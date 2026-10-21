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
import android.os.SystemClock
import android.util.Log
import androidx.core.app.NotificationCompat
import io.github.abdoeid.phonecam.MainActivity
import io.github.abdoeid.phonecam.R
import io.github.abdoeid.phonecam.control.ControlChannel
import io.github.abdoeid.phonecam.control.ControlCommandListener
import io.github.abdoeid.phonecam.control.PairingCode
import io.github.abdoeid.phonecam.control.TrustedPcs
import io.github.abdoeid.phonecam.rtp.RtpVideoSink
import io.github.abdoeid.phonecam.transport.AoaTransport
import io.github.abdoeid.phonecam.transport.DiscoveryResponder
import io.github.abdoeid.phonecam.transport.OutputStreamVideoSink
import java.io.IOException
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
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
  private val userCommands = Executors.newSingleThreadExecutor { task -> Thread(task, "Capture-user-actions") }

  // The control channel is durable: it outlives any single capture session, so a PC can
  // command a phone that is sitting idle (start a wireless session, ask for a keyframe) and
  // can come back later without the phone having to rebind anything. Owned here, not by
  // CaptureController, whose lifetime is one capture.
  private var controlChannel: ControlChannel? = null
  private var controlThread: Thread? = null
  private val controlRunning = AtomicBoolean(false)

  // Answers the PC's "is there a PhoneCam here?" probe, so a wireless session doesn't
  // need the phone's address typed on the PC. Runs for as long as this service does --
  // the same lifetime the control channel has, so anything that can control the phone can
  // also find it.
  private var discoveryResponder: DiscoveryResponder? = null

  // True while the current capture session exists because the PC asked for it (StartVideo), as
  // opposed to the user tapping Start on the phone. It decides two things: whether the PC may
  // replace the session, and whether the session should end when the PC's control channel drops.
  @Volatile
  private var sessionStartedByPc = false

  // The transport the current session is running on (VideoTransportMode), so a repeated
  // request for the same mode doesn't restart it.
  @Volatile
  private var sessionMode = -1

  // Owned here, not by MainActivity (see its class doc's AOA note): ACTION_START_AOA gives
  // this the same foreground-service/wake-lock/error-handling treatment ACTION_START already gets,
  // instead of a separate ad-hoc lifecycle tied to the Activity. Non-null only while an
  // AOA-triggered session is live; closed in stopCaptureInternal()/onDestroy() alongside the
  // controller.
  private var aoaTransport: AoaTransport? = null

  // Durable user intent, independent of camera errors, transport retries and process recreation.
  private val sessionPrefs by lazy { getSharedPreferences("phonecam_session", MODE_PRIVATE) }
  @Volatile var userStopped = false
    private set
  val connectedPc: String get() {
    val channel = controlChannel ?: return ""
    if (!channel.isConnected) return ""
    return TrustedPcs(this).list().find { it.id == channel.clientId }?.name ?: "PC"
  }
  private fun persistStopIntent(value: Boolean) {
    userStopped = value
    sessionPrefs.edit().putBoolean("stopped", value).commit()
    controlChannel?.sendStreamIntent()
  }

  // The camera-free heartbeat the host uses to detect a live peer. Answering it
  // must not open the camera, so the phone only republishes its stop intent.
  fun answerIntentProbe() {
    controlChannel?.sendStreamIntent()
  }

  val state: CaptureState
    get() = controller.state

  val elapsedSeconds: Double
    get() = controller.elapsedSeconds

  val measuredFps: Double
    get() = controller.measuredFps
  val hasVideoFrames: Boolean get() = controller.framesEncoded > 0

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

  override fun onBind(intent: Intent): IBinder {
    if (userStopped) {
      stopForeground(STOP_FOREGROUND_REMOVE)
    } else {
      // A PC-started capture must outlive the UI binding too. A foreground
      // notification alone does not make a bound service a started service.
      startService(Intent(this, CaptureService::class.java).setAction(ACTION_KEEP_ALIVE))
    }
    return binder
  }

  override fun onCreate() {
    super.onCreate()
    controller = CaptureController(this)
    TrustedPcs.init(this)
    userStopped = sessionPrefs.getBoolean("stopped", false)
    createNotificationChannel()
    startControlChannel()
    startDiscovery()
    // Promote before serving any Start. Explicit Stop later removes foreground
    // status; only the visible UI's binding then keeps control/discovery alive.
    // Already-running capture remains foreground and survives screen lock.
    startForegroundInternal()
  }

  /** Brings up the discovery responder (see [DiscoveryResponder] for why it exists). */
  private fun startDiscovery() {
    val responder = DiscoveryResponder(CONTROL_PORT, VIDEO_PORT, TrustedPcs(this))
    discoveryResponder = responder
    responder.start()
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
    // The pairing code the PC has to prove it knows. A session over the local USB link is
    // trusted without one; a network session has to answer the challenge.
    val pairing = PairingCode(this)
    channel.pairingCodeProvider = { pairing.code }
    channel.trustedPcKeyProvider = { TrustedPcs(this).key(it) }
    channel.stoppedProvider = { userStopped }
    // While the phone scans a PC's QR it must not accept a network login (the camera
    // is being used for the scan). The block is time-limited: an abandoned scan must
    // not leave the phone refusing every PC afterwards.
    channel.networkLoginBlocked = { isScanningPcQr() }
    // The controller pushes state changes (CurrentSettings/Stats/LensChanged) back to the PC
    // through this channel, so it needs the reference.
    controller.bindControlChannel(channel)
    // When the PC's control connection ends, a session that only existed because the PC asked
    // for it ends with it. Without this, a wireless session would outlive the PC: a UDP sink
    // never fails on its own, so the phone would keep the camera open, streaming into the void,
    // and refuse to start a new session ("already streaming").
    channel.onSessionEnded = {
      if (sessionStartedByPc) {
        Log.i(TAG, "the PC disconnected -- stopping the session it asked for")
        stopCaptureInternal()
      } else {
        // A phone-started capture can also have an authenticated network viewer.
        // Its video socket must not outlive the control session that authorized it.
        controller.disconnectNetworkVideo()
      }
    }
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

      override fun onStartVideo(mode: Int, width: Int, height: Int, fps: Int, bitrateBps: Int, rtpPort: Int, explicitStart: Boolean): Byte =
        startSession(mode, width, height, fps, bitrateBps, rtpPort, explicitStart)

      override fun onStopVideo(userRequested: Boolean): Byte {
        if (userRequested) persistStopIntent(true)
        return stopWirelessSession()
      }
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
  @Volatile private var scanningPcQr = false
  @Volatile private var scanDeadlineMs = 0L
  @Synchronized private fun startSession(mode: Int, width: Int, height: Int, fps: Int, bitrateBps: Int, rtpPort: Int, explicitStart: Boolean = false): Byte {
    if (isScanningPcQr()) return AckResult.Busy
    if (explicitStart) persistStopIntent(false)
    if (userStopped) { controlChannel?.sendStreamIntent(); return AckResult.Busy }
    if (controller.state != CaptureState.IDLE) {
      if (sessionStartedByPc && sessionMode == mode) {
        // Already running exactly what was asked for. The PC re-asks periodically (it can't
        // always see that frames are arriving), and that must not restart the session.
        Log.i(TAG, "the session is already running in this mode")
        return AckResult.Ok
      }
      // Something else is streaming. The PC is asking for a specific transport, so it
      // wins: the existing session is dropped and the new one starts. (The old session is
      // often just the UI's error-retry waiting for a video connection that isn't coming --
      // it would otherwise block every later request.)
      Log.i(TAG, "replacing the current session (${controller.state}) with the PC's request")
      stopCaptureInternal()
    }
    stopRequested.set(false)
    lastError = null
    sessionStartedByPc = true
    sessionMode = mode
    return try {
      startService(Intent(this, CaptureService::class.java).setAction(ACTION_KEEP_ALIVE))
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
      updateNotification(streaming = true)
      AckResult.Ok
    } catch (e: Exception) {
      Log.e(TAG, "failed to start the session", e)
      lastError = e
      AckResult.Error
    }
  }

  @Synchronized private fun stopWirelessSession(): Byte {
    stopRequested.set(true)
    stopCaptureInternal()
    return AckResult.Ok
  }

  fun beginQrScan() {
    scanningPcQr = true
    scanDeadlineMs = SystemClock.elapsedRealtime() + SCAN_LOCK_TIMEOUT_MS
    stopRequested.set(true)
    lastError = null
    controller.stop()
    sessionStartedByPc = false
    sessionMode = -1
    releaseWakeLock()
    controlChannel?.disconnectClient()
  }

  fun endQrScan() {
    scanningPcQr = false
  }

  private fun isScanningPcQr() = scanningPcQr && SystemClock.elapsedRealtime() < scanDeadlineMs

  fun forgetPc(id: String) {
    TrustedPcs(this).forget(id)
    controlChannel?.revokeForgottenPc()
  }

  @Synchronized fun pauseByUser() {
    persistStopIntent(true)
    stopRequested.set(true)
    lastError = null
    stopCaptureInternal()
  }

  @Synchronized fun resumeByUser(): Boolean {
    persistStopIntent(false)
    lastError = null
    // A network PC drives the session itself (it knows the transport and answers with
    // video); a local USB link has no such driver, so the phone starts the camera itself.
    return controlChannel?.isConnected == true && controlChannel?.peerAddress != null
  }

  // CaptureController.stop() closes the video socket before it stops the encoder (see its own
  // comments) -- so the encoder's drain thread can hit a write failure ("Socket closed") as a
  // direct, expected consequence of a normal user-requested stop, and that failure calls the same
  // onError callback a genuine mid-stream error would. Track whether a stop was requested so
  // onCaptureError can tell "this is just stop() tearing down" from "this is a real error" and
  // ignore the former -- otherwise a plain Cancel tap could surface a spurious "Socket closed"
  // error during teardown, confirmed live.
  private val stopRequested = AtomicBoolean(false)

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    // Camera teardown, durable preference writes and control socket I/O must not block the UI
    // (or trigger NetworkOnMainThreadException). Preserve rapid Stop/Start ordering off-main.
    userCommands.execute { handleCommand(intent, flags, startId) }
    return START_NOT_STICKY
  }

  @Synchronized private fun handleCommand(intent: Intent?, flags: Int, startId: Int): Int {
    if (isScanningPcQr() && (intent?.action == ACTION_START || intent?.action == ACTION_START_AOA)) return START_NOT_STICKY
    when (intent?.action) {
      ACTION_KEEP_ALIVE -> {
        if (userStopped) stopSelf() else startForegroundInternal()
      }
      ACTION_START -> {
        startForegroundInternal()
        // A network PC drives the session itself (it knows the transport and answers with
        // video), so the phone only clears its stop intent and lets the PC resume.
        if (resumeByUser()) return START_NOT_STICKY
        val accessory = getSystemService(UsbManager::class.java).accessoryList?.firstOrNull()
        if (accessory != null) {
          return onStartCommand(intent.setAction(ACTION_START_AOA).putExtra(EXTRA_ACCESSORY, accessory), flags, startId)
        }
        // Ignore a redundant Start while another capture is already live.
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
        if (userStopped) return START_NOT_STICKY
        // Ignore a redundant accessory-attach dispatch (e.g. MainActivity re-firing from its
        // accessoryList fallback on a plain re-launch) while a session is already live -- matches
        // the old TEMPORARY wiring's `if (aoaTransport.isOpen) return` guard, moved here now that
        // this owns the transport.
        if (controller.state != CaptureState.IDLE) return START_NOT_STICKY
        val accessory =
          if (Build.VERSION.SDK_INT >= 33) {
            intent.getParcelableExtra(EXTRA_ACCESSORY, UsbAccessory::class.java)
          } else {
            @Suppress("DEPRECATION") intent.getParcelableExtra(EXTRA_ACCESSORY)
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
        pauseByUser()
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
    sessionStartedByPc = false
    sessionMode = -1
    releaseWakeLock()
    if (userStopped) {
      // While the UI is bound, control stays available for one-tap Start. Once
      // the UI closes/unbinds, Android destroys this now-unstarted service.
      stopForeground(STOP_FOREGROUND_REMOVE)
      stopSelf()
    } else {
      updateNotification(streaming = false)
    }
  }

  private fun startForegroundInternal() {
    val notification = buildNotification(streaming = controller.state == CaptureState.STREAMING)
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

  /** Refreshes the notification's text -- "ready" while idle, "streaming" while capturing. */
  private fun updateNotification(streaming: Boolean) {
    try {
      val manager = getSystemService(NotificationManager::class.java)
      manager.notify(NOTIFICATION_ID, buildNotification(streaming))
    } catch (e: Exception) {
      Log.w(TAG, "could not update the notification", e)
    }
  }

  private fun buildNotification(streaming: Boolean): Notification {
    val openIntent =
      PendingIntent.getActivity(
        this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
      )
    return NotificationCompat.Builder(this, CHANNEL_ID)
      .setContentTitle(if (userStopped) "PhoneCam is stopped" else if (streaming) "PhoneCam is streaming" else "PhoneCam is ready")
      .setContentText("Tap to return to the app")
      .setSmallIcon(R.mipmap.ic_launcher)
      .setContentIntent(openIntent)
      .setOngoing(true)
      .setOnlyAlertOnce(true)
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
    userCommands.shutdownNow()
    userCommands.awaitTermination(2, TimeUnit.SECONDS)
    controlRunning.set(false)
    controlChannel?.stop()
    if (controlThread !== Thread.currentThread()) {
      controlThread?.join(1000)
    }
    controlThread = null
    controlChannel = null
    discoveryResponder?.stop()
    discoveryResponder = null
    controller.stop()
    aoaTransport?.close()
    aoaTransport = null
    releaseWakeLock()
    stopForeground(STOP_FOREGROUND_REMOVE)
    super.onDestroy()
  }

  companion object {
    private const val TAG = "CaptureService"
    private const val CONTROL_RETRY_DELAY_MS = 2000L

    // How long a QR scan may hold off network logins. An abandoned scan (the user
    // backs out without scanning) must not leave the phone refusing every PC.
    private const val SCAN_LOCK_TIMEOUT_MS = 120_000L

    // The ports the phone listens on, reported to the PC by the discovery responder.
    // Must match windows/host/control/ControlTransport.cpp (27184) and
    // windows/host/transport/AdbTransport.cpp (27183).
    private const val CONTROL_PORT = 27184
    private const val VIDEO_PORT = 27183
    const val ACTION_START = "io.github.abdoeid.phonecam.action.START"
    const val ACTION_START_AOA = "io.github.abdoeid.phonecam.action.START_AOA"
    const val ACTION_STOP = "io.github.abdoeid.phonecam.action.STOP"
    private const val ACTION_KEEP_ALIVE = "io.github.abdoeid.phonecam.action.KEEP_ALIVE"
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
