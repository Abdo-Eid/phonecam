package io.github.abdoeid.phonecam.capture

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.hardware.camera2.CameraCharacteristics
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import androidx.core.app.NotificationCompat
import io.github.abdoeid.phonecam.MainActivity
import io.github.abdoeid.phonecam.R
import io.github.abdoeid.phonecam.control.ControlChannel
import io.github.abdoeid.phonecam.control.ControlCommandListener
import io.github.abdoeid.phonecam.rtp.RtpVideoSink
import io.github.abdoeid.phonecam.transport.AoaTransport
import io.github.abdoeid.phonecam.transport.OutputStreamVideoSink
import java.io.IOException
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicBoolean
import phonecam.control.AckResult
import phonecam.control.VideoTransportMode

// A camera-type foreground service (android:foregroundServiceType="camera" in the manifest) is
// what lets Android keep this process -- and its camera access -- alive while the app is
// backgrounded or the screen is off. Previously CaptureController lived directly in
// MainScreenViewModel, whose lifecycle is tied to MainActivity: backgrounding the app could get
// the process frozen or killed by Doze/App Standby, silently ending capture with no way to
// recover short of reopening the app. Owning CaptureController here instead decouples capture
// from the Activity's lifecycle entirely -- MainScreenViewModel now just sends start/stop intents
// and polls this service's state through a bound connection.
class CaptureService : Service() {
  private lateinit var controller: CaptureController
  private var wakeLock: PowerManager.WakeLock? = null

  // The control channel is durable: it outlives any single capture session, so a PC can
  // command a phone that is sitting idle (start a wireless session, ask for a keyframe) and
  // can come back later without the phone having to rebind anything. Owned here, not by
  // CaptureController, whose lifetime is one capture.
  private var controlChannel: ControlChannel? = null
  private var controlThread: Thread? = null
  private val controlRunning = AtomicBoolean(false)

  // Owned here, not by MainActivity (see its class doc's AOA note): ACTION_START_AOA gives
  // this the same foreground-service/wake-lock/error-handling treatment ACTION_START already gets,
  // instead of a separate ad-hoc lifecycle tied to the Activity. Non-null only while an
  // AOA-triggered session is live; closed in stopCaptureInternal()/onDestroy() alongside the
  // controller.
  private var aoaTransport: AoaTransport? = null

  val state: CaptureState
    get() = controller.state

  val elapsedSeconds: Double
    get() = controller.elapsedSeconds

  val measuredFps: Double
    get() = controller.measuredFps

  // Polled by MainScreenViewModel alongside [state] rather than delivered via a registered
  // callback -- a callback registered only after binding completes could miss an error that
  // occurs in the (unlikely but possible) window between ACTION_START being dispatched and the
  // bind finishing. Cleared at the start of every new ACTION_START.
  @Volatile
  var lastError: Throwable? = null
    private set

  inner class LocalBinder : Binder() {
    fun getService(): CaptureService = this@CaptureService
  }

  private val binder = LocalBinder()

  override fun onBind(intent: Intent): IBinder = binder

  override fun onCreate() {
    super.onCreate()
    controller = CaptureController(this)
    createNotificationChannel()
    startControlChannel()
  }

  /**
   * Brings up the durable control channel on a background thread.
   *
   * It is bound once and then serves every PC that connects (a disconnect and reconnect
   * doesn't rebind anything), which is what lets the PC drive a session from a phone that
   * isn't streaming yet.
   */
  private fun startControlChannel() {
    val channel = ControlChannel(this, controlListener)
    controlChannel = channel
    // The controller pushes state changes (CurrentSettings/Stats/LensChanged) back to the PC
    // through this channel, so it needs the reference.
    controller.bindControlChannel(channel)
    controlRunning.set(true)
    controlThread =
      Thread(
          {
            while (controlRunning.get()) {
              try {
                channel.runSessions { !controlRunning.get() }
              } catch (e: IOException) {
                if (controlRunning.get()) {
                  // Most likely the listeners are still held by a previous instance of the app
                  // ("Address already in use") -- back off instead of spinning on it.
                  Log.w(TAG, "control channel could not start, retrying", e)
                }
              }
              if (controlRunning.get()) {
                try {
                  Thread.sleep(CONTROL_RETRY_DELAY_MS)
                } catch (_: InterruptedException) {
                  Thread.currentThread().interrupt()
                  break
                }
              }
            }
          },
          "ControlChannel-sessions",
        )
        .apply { start() }
  }

  /**
   * Routes PC commands: camera commands go to the controller (which applies them to whichever
   * camera/encoder is live), session commands are handled here (this service owns the capture
   * lifecycle).
   */
  private val controlListener =
    object : ControlCommandListener {
      override fun onSetZoomRatio(ratio: Float): Byte = controller.cameraCommandListener.onSetZoomRatio(ratio)

      override fun onSetEv(steps: Int): Byte = controller.cameraCommandListener.onSetEv(steps)

      override fun onSetTorch(on: Boolean): Byte = controller.cameraCommandListener.onSetTorch(on)

      override fun onSetFocusMode(mode: Byte): Byte = controller.cameraCommandListener.onSetFocusMode(mode)

      override fun onTapToFocus(nx: Float, ny: Float, regionSize: Float): Byte =
        controller.cameraCommandListener.onTapToFocus(nx, ny, regionSize)

      override fun onRequestKeyframe(): Byte = controller.cameraCommandListener.onRequestKeyframe()

      override fun onSetBitrate(bitrateBps: Int): Byte = controller.cameraCommandListener.onSetBitrate(bitrateBps)

      override fun onSetLens(cameraId: String): Byte = controller.cameraCommandListener.onSetLens(cameraId)

      override fun onStartVideo(mode: Int, width: Int, height: Int, fps: Int, bitrateBps: Int, rtpPort: Int): Byte =
        startSession(mode, width, height, fps, bitrateBps, rtpPort)

      override fun onStopVideo(): Byte = stopWirelessSession()
    }

  /**
   * Starts a session the PC asked for, over the transport it named.
   *
   * Framed TCP: the phone binds its video socket and waits for the PC to connect -- the same
   * flow the phone used to start on its own, now triggered by the PC. That matters because a
   * phone-side start can't be automated (and shouldn't have to be).
   *
   * RTP: capture and H.264 go straight to the PC as UDP. The PC's address comes from the
   * control connection itself (the phone learns it from the socket the PC connected on) --
   * there is no address to type on the phone and no discovery protocol to speak of.
   */
  private fun startSession(mode: Int, width: Int, height: Int, fps: Int, bitrateBps: Int, rtpPort: Int): Byte {
    if (controller.state != CaptureState.IDLE) {
      // Already streaming on another transport. Two video paths at once would mean two
      // encoders fighting over the camera, so the existing one wins and the PC is told.
      Log.w(TAG, "StartVideo ignored: the phone is already streaming")
      return AckResult.Busy
    }
    stopRequested.set(false)
    lastError = null
    return try {
      if (mode == VideoTransportMode.Rtp.toInt()) {
        val peer = controlChannel?.peerAddress
        if (peer.isNullOrEmpty()) {
          // The PC is on the adb-forwarded abstract socket, so there's no address to stream to.
          // An RTP session needs a direct TCP control connection.
          Log.w(TAG, "RTP session ignored: the PC connected over adb, so there is no address to stream to")
          return AckResult.Error
        }
        val sink = RtpVideoSink(InetAddress.getByName(peer), rtpPort)
        startForegroundInternal()
        acquireWakeLock()
        controller.start(width, height, fps, bitrateBps, ::onCaptureError, videoPacketSink = sink)
        Log.i(TAG, "RTP session started: streaming to $peer:$rtpPort")
      } else {
        // Framed TCP: no sink, so the controller binds the video socket and waits for the PC.
        startForegroundInternal()
        acquireWakeLock()
        controller.start(width, height, fps, bitrateBps, ::onCaptureError)
        Log.i(TAG, "framed-TCP session started, waiting for the PC to connect")
      }
      AckResult.Ok
    } catch (e: Exception) {
      Log.e(TAG, "failed to start the session", e)
      lastError = e
      AckResult.Error
    }
  }

  private fun stopWirelessSession(): Byte {
    if (controller.state == CaptureState.IDLE) return AckResult.Ok
    stopRequested.set(true)
    stopCaptureInternal()
    return AckResult.Ok
  }

  // CaptureController.stop() closes the video socket before it stops the encoder (see its own
  // comments) -- so the encoder's drain thread can hit a write failure ("Socket closed") as a
  // direct, expected consequence of a normal user-requested stop, and that failure calls the same
  // onError callback a genuine mid-stream error would. Track whether a stop was requested so
  // onCaptureError can tell "this is just stop() tearing down" from "this is a real error" and
  // ignore the former -- otherwise a plain Cancel tap could surface a spurious "Socket closed"
  // error (and even trigger MainScreen's Error-path auto-retry), confirmed live.
  private val stopRequested = AtomicBoolean(false)

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    when (intent?.action) {
      ACTION_START -> {
        // Same "ignore a redundant dispatch while already live" guard as ACTION_START_AOA below --
        // without it, MainScreen's auto-start effect (which fires from a UI still showing stale
        // Idle state) can send this while an AOA-triggered session is already running, since the
        // two paths are otherwise unaware of each other.
        if (controller.state != CaptureState.IDLE) return START_NOT_STICKY
        val width = intent.getIntExtra(EXTRA_WIDTH, 1280)
        val height = intent.getIntExtra(EXTRA_HEIGHT, 720)
        val fps = intent.getIntExtra(EXTRA_FPS, 30)
        val bitrateBps = intent.getIntExtra(EXTRA_BITRATE, 4_000_000)
        val lensFacing = intent.getIntExtra(EXTRA_LENS_FACING, CameraCharacteristics.LENS_FACING_BACK)
        stopRequested.set(false)
        lastError = null
        startForegroundInternal()
        acquireWakeLock()
        controller.start(width, height, fps, bitrateBps, ::onCaptureError, lensFacing)
      }
      ACTION_START_AOA -> {
        // Ignore a redundant accessory-attach dispatch (e.g. MainActivity re-firing from its
        // accessoryList fallback on a plain re-launch) while a session is already live -- matches
        // the old TEMPORARY wiring's `if (aoaTransport.isOpen) return` guard, moved here now that
        // this owns the transport.
        if (controller.state != CaptureState.IDLE) return START_NOT_STICKY
        val accessory =
          if (Build.VERSION.SDK_INT >= 33) {
            intent?.getParcelableExtra(EXTRA_ACCESSORY, UsbAccessory::class.java)
          } else {
            @Suppress("DEPRECATION") intent?.getParcelableExtra(EXTRA_ACCESSORY)
          }
        if (accessory == null) return START_NOT_STICKY
        stopRequested.set(false)
        lastError = null
        startForegroundInternal()
        acquireWakeLock()
        val transport = AoaTransport()
        val usbManager = getSystemService(UsbManager::class.java)
        if (!transport.open(usbManager, accessory)) {
          lastError = IOException("Failed to open USB accessory")
          stopCaptureInternal()
          return START_NOT_STICKY
        }
        aoaTransport = transport
        // 1920x1080 to match the vcam's declared stream size (windows/vcam/MediaStream.cpp,
        // matching the vcam's declared stream size) -- same reasoning as MainScreenViewModel's
        // Start-button defaults: a mismatch
        // here silently stretches the image on the PC side.
        controller.start(
          width = 1920,
          height = 1080,
          fps = 30,
          bitrateBps = 6_000_000,
          onError = ::onCaptureError,
          videoPacketSink = OutputStreamVideoSink(transport.videoOutput),
        )
      }
      ACTION_STOP -> {
        stopRequested.set(true)
        stopCaptureInternal()
      }
    }
    return START_NOT_STICKY
  }

  private fun onCaptureError(error: Throwable) {
    Log.e(TAG, "capture error: $error")
    if (stopRequested.get()) return
    lastError = error
    stopCaptureInternal()
  }

  private fun stopCaptureInternal() {
    controller.stop()
    aoaTransport?.close()
    aoaTransport = null
    releaseWakeLock()
    stopForeground(STOP_FOREGROUND_REMOVE)
    stopSelf()
  }

  private fun startForegroundInternal() {
    val notification = buildNotification()
    try {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
      } else {
        startForeground(NOTIFICATION_ID, notification)
      }
    } catch (e: Exception) {
      // Android 12+ refuses a foreground promotion started from the background. That must not
      // take the session down with it: capture continues, it just isn't protected from the
      // process being frozen or killed while the app isn't on screen.
      Log.w(TAG, "startForeground() was refused (app in the background?) -- continuing without it", e)
    }
  }

  private fun buildNotification(): Notification {
    val openIntent =
      PendingIntent.getActivity(
        this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
      )
    return NotificationCompat.Builder(this, CHANNEL_ID)
      .setContentTitle("PhoneCam is streaming")
      .setContentText("Tap to return to the app")
      .setSmallIcon(R.mipmap.ic_launcher)
      .setContentIntent(openIntent)
      .setOngoing(true)
      .build()
  }

  private fun createNotificationChannel() {
    val channel = NotificationChannel(CHANNEL_ID, "PhoneCam streaming", NotificationManager.IMPORTANCE_LOW)
    getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
  }

  // Capped at 12h rather than an indefinite acquire() -- belt-and-suspenders against a code path
  // that somehow skips releaseWakeLock() draining the battery forever; a genuine session lasting
  // that long will simply re-acquire on its next ACTION_START-driven poll, since [state] would
  // still read STREAMING/WAITING and the UI would surface that as still-running.
  private fun acquireWakeLock() {
    if (wakeLock != null) return
    val pm = getSystemService(POWER_SERVICE) as PowerManager
    wakeLock =
      pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PhoneCam:CaptureWakeLock").apply {
        acquire(12 * 60 * 60 * 1000L)
      }
  }

  private fun releaseWakeLock() {
    wakeLock?.let { if (it.isHeld) it.release() }
    wakeLock = null
  }

  override fun onDestroy() {
    controlRunning.set(false)
    controlChannel?.stop()
    if (controlThread !== Thread.currentThread()) {
      controlThread?.join(1000)
    }
    controlThread = null
    controlChannel = null
    controller.stop()
    aoaTransport?.close()
    aoaTransport = null
    releaseWakeLock()
    super.onDestroy()
  }

  companion object {
    private const val TAG = "CaptureService"
    private const val CONTROL_RETRY_DELAY_MS = 2000L
    const val ACTION_START = "io.github.abdoeid.phonecam.action.START"
    const val ACTION_START_AOA = "io.github.abdoeid.phonecam.action.START_AOA"
    const val ACTION_STOP = "io.github.abdoeid.phonecam.action.STOP"
    const val EXTRA_WIDTH = "width"
    const val EXTRA_HEIGHT = "height"
    const val EXTRA_FPS = "fps"
    const val EXTRA_BITRATE = "bitrateBps"
    const val EXTRA_LENS_FACING = "lensFacing"
    const val EXTRA_ACCESSORY = "accessory"
    private const val CHANNEL_ID = "phonecam_capture"
    private const val NOTIFICATION_ID = 1
  }
}
