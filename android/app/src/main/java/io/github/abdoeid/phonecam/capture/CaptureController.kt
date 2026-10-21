package io.github.abdoeid.phonecam.capture

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraMetadata
import android.os.PowerManager
import android.os.SystemClock
import io.github.abdoeid.phonecam.control.ControlChannel
import io.github.abdoeid.phonecam.control.ControlCommandListener
import io.github.abdoeid.phonecam.encode.H264Encoder
import io.github.abdoeid.phonecam.transport.OutputStreamVideoSink
import io.github.abdoeid.phonecam.transport.VideoPacketSink
import io.github.abdoeid.phonecam.transport.VideoSocketServer
import java.io.IOException
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.max
import kotlin.math.min
import phonecam.control.AckResult
import phonecam.control.ThermalStatus
import phonecam.wire.AfMode

enum class CaptureState { IDLE, WAITING_FOR_CONNECTION, STREAMING }

// Adaptive bitrate never drops below this floor -- degrading further than ~1Mbps produces
// visibly unusable quality regardless of thermal/dropped-frame pressure.
private const val MIN_ADAPTIVE_BITRATE_BPS = 1_000_000

/**
 * Wires CameraCapture's output straight into H264Encoder's input Surface, which emits
 * wire-framed packets (docs/wire-protocol.md) to [videoPacketSink] -- the connected PC's video
 * channel (USB socket, AOA, RTP). Camera and encoder don't start until a
 * consumer is actually there -- stream transports block in [accept], and the RTP path is started
 * by an explicit StartVideo command -- so nothing captures or encodes into the void.
 */
class CaptureController(private val context: Context) {
  private var encoder: H264Encoder? = null
  private var camera: CameraCapture? = null
  @Volatile private var videoServer: VideoSocketServer? = null
  private var startTimeMs: Long = 0
  private var acceptThread: Thread? = null

  // Owned by CaptureService (durable across capture sessions): set once via
  // [bindControlChannel]. The per-session control channel this class used to create itself
  // couldn't outlive a capture stop(), which blocked every PC-driven session command (there
  // would be nothing listening to receive it).
  @Volatile
  private var controlChannel: ControlChannel? = null

  // This session's packet sink, closed by [stop] -- RtpVideoSink owns a UDP socket that must be
  // released; stream-backed sinks are no-ops (their owners close the sockets).
  @Volatile
  private var videoPacketSink: VideoPacketSink? = null

  private val stopRequested = AtomicBoolean(false)

  // A fresh token per start() cycle, captured by that cycle's thread closures -- NOT a single
  // shared field like [stopRequested] or [state]. Those get reset by the very next start() call,
  // which can race a slow-to-notice previous cycle: if Cancel fires start() again quickly (the
  // Idle branch of MainScreen's auto-start effect does this immediately, no delay), the old
  // accept thread's socket-bind retry loop could still be running in the background with no way
  // to learn it was cancelled, silently orphaned while a new thread races it for the same
  // abstract socket name -- confirmed live as a "Cancel -> stuck in Address already in use" loop.
  // Each cycle's own token is set to true by stop() and only ever read by that cycle's own
  // threads, so there's no shared-state race to lose.
  private var cancelToken = AtomicBoolean(true) // "cancelled": true when nothing is running

  @Volatile
  var state: CaptureState = CaptureState.IDLE
    private set

  // Tracks the last value accepted for each live control -- CameraCapture doesn't expose readers
  // for its CaptureRequest.Builder state, and this is also exactly what CurrentSettings pushes
  // out after every successful command (docs/control-protocol.md's reconciliation discipline).
  private var lastWidth = 0
  private var lastHeight = 0
  private var lastFps = 0
  private var lastBitrateBps = 0
  private var lastZoom = 1.0f
  private var lastEv = 0
  private var lastFocusMode: Byte = AfMode.Continuous
  private var lastTorch = false

  // Ceiling the adaptive-bitrate loop (below) won't exceed -- the last explicitly requested
  // value, whether from start()'s caller or a PC SetBitrate command. Distinct from
  // [lastBitrateBps], which tracks whatever's actually applied right now (possibly reduced by
  // adaptation); a PC-issued SetBitrate updates both, so it always wins over adaptation rather
  // than being silently overwritten by the next stats tick.
  @Volatile
  private var targetBitrateBps = 0

  private var statsThread: Thread? = null

  // Public because the durable control channel in CaptureService (which owns the receive thread
  // this listener runs on) delegates camera commands here. Dispatches PC -> phone commands
  // (docs/control-protocol.md) onto whichever camera/encoder instance is live right now;
  // camera/encoder is null until the video accept-thread finishes standing them up, and briefly
  // null again mid lens-switch -- both are real "not ready yet" states, reported as Ack{Busy}
  // rather than silently dropped or crashing.
  val cameraCommandListener =
    object : ControlCommandListener {
      override fun onSetZoomRatio(ratio: Float): Byte =
        ackFor(camera?.setZoomRatio(ratio)) { lastZoom = ratio }

      override fun onSetEv(steps: Int): Byte = ackFor(camera?.setEv(steps)) { lastEv = steps }

      override fun onSetTorch(on: Boolean): Byte = ackFor(camera?.setTorch(on)) { lastTorch = on }

      override fun onSetFocusMode(mode: Byte): Byte {
        val camera2Mode = afModeToCamera2(mode) ?: return AckResult.OutOfRange
        return ackFor(camera?.setFocusMode(camera2Mode)) { lastFocusMode = mode }
      }

      override fun onTapToFocus(nx: Float, ny: Float, regionSize: Float): Byte =
        ackFor(camera?.tapToFocus(nx, ny, regionSize))

      override fun onRequestKeyframe(): Byte {
        val enc = encoder ?: return AckResult.Busy
        enc.requestKeyframe()
        return AckResult.Ok
      }

      override fun onSetBitrate(bitrateBps: Int): Byte {
        val enc = encoder ?: return AckResult.Busy
        enc.setBitrate(bitrateBps)
        lastBitrateBps = bitrateBps
        targetBitrateBps = bitrateBps // an explicit PC request re-anchors the adaptive ceiling too
        pushCurrentSettings()
        return AckResult.Ok
      }

      override fun onSetLens(cameraId: String): Byte {
        val cam = camera ?: return AckResult.Busy
        cam.switchLens(cameraId) { /* async failure surfaces via the next command's Busy/Ack, or a future Error message */ }
        return AckResult.Ok
      }
    }

  private fun ackFor(applied: Boolean?, onApplied: () -> Unit = {}): Byte {
    if (applied != true) return AckResult.Busy
    onApplied()
    pushCurrentSettings()
    return AckResult.Ok
  }

  private fun pushCurrentSettings() {
    controlChannel?.sendCurrentSettings(
      camera?.currentCameraId ?: "", lastWidth, lastHeight, lastFps, lastBitrateBps, lastZoom, lastFocusMode,
      lastEv, lastTorch,
    )
  }

  /** @see controlChannel doc comment. Set once by CaptureService at bind time; not unset on stop. */
  fun bindControlChannel(channel: ControlChannel) {
    controlChannel = channel
  }

  fun disconnectNetworkVideo() {
    videoServer?.closeNetworkClient()
  }

  /**
   * [videoPacketSink], when non-null, replaces [VideoSocketServer] entirely as the video packet
   * destination -- used by transports whose session is already established by the time [start] is
   * called, so there is no accept-and-wait-for-a-PC-to-connect step to do (AOA: the
   * accessory pipe is already open; wireless: RTP sends to the PC's address, learned
   * from the control channel, and there is nothing to accept). When null (the default,
   * USB path), behavior is unchanged.
   */
  fun start(
    width: Int,
    height: Int,
    fps: Int,
    bitrateBps: Int,
    onError: (Throwable) -> Unit,
    lensFacing: Int = CameraCharacteristics.LENS_FACING_BACK,
    videoPacketSink: VideoPacketSink? = null,
  ) {
    stopRequested.set(false)
    this.videoPacketSink = videoPacketSink
    val myCancelToken = AtomicBoolean(false)
    cancelToken = myCancelToken
    state = CaptureState.WAITING_FOR_CONNECTION
    lastWidth = width
    lastHeight = height
    lastFps = fps
    lastBitrateBps = bitrateBps
    lastZoom = 1.0f
    lastEv = 0
    lastFocusMode = AfMode.Continuous
    lastTorch = false
    targetBitrateBps = bitrateBps

    acceptThread =
      Thread({
          // Binding also happens on this thread (not eagerly in start() on
          // the caller's/main thread): a rapid Cancel-then-Start can ask to
          // rebind the same abstract socket name before the OS finishes
          // releasing the just-closed previous session's socket, and
          // VideoSocketServer.open() already retries that briefly -- doing
          // it here keeps the UI thread from blocking on those retries too.
          //
          // Every bail-out check below reads myCancelToken (this cycle's own token), never the
          // shared `state` field -- state gets overwritten by the very next start() call, so it
          // can't reliably tell this thread "you were cancelled" if a new cycle already began.
          val out: VideoPacketSink
          if (videoPacketSink != null) {
            // Stream-external transports: the destination already exists, nothing to bind/accept
            // (AOA's open accessory pipe, or RTP where the PC's address came from the control
            // channel and there is no accept step at all).
            out = videoPacketSink
          } else {
            val server = VideoSocketServer(authenticateTcp = { socket ->
              controlChannel?.authenticateVideoConnection(socket, myCancelToken::get) == true
            })
            try {
              server.open(isCancelled = myCancelToken::get)
            } catch (e: IOException) {
              if (!myCancelToken.get()) {
                state = CaptureState.IDLE
                onError(e)
              }
              return@Thread
            }
            videoServer = server
            if (myCancelToken.get()) {
              // stop() raced us during open() -- bail before accept().
              server.close()
              return@Thread
            }

            out =
              try {
                OutputStreamVideoSink(server.accept(isCancelled = myCancelToken::get))
              } catch (e: IOException) {
                // cancelled means stop() closed the socket deliberately -- not a real error.
                if (!myCancelToken.get()) {
                  state = CaptureState.IDLE
                  onError(e)
                }
                return@Thread
              }
            if (myCancelToken.get()) {
              // stop() raced us right as the PC connected -- bail without starting anything.
              try {
                out.close()
              } catch (_: IOException) {
                // already torn down
              }
              return@Thread
            }
          }

          val enc = H264Encoder(width, height, fps, bitrateBps)
          encoder = enc
          enc.start(out) { writeError ->
            if (!myCancelToken.get()) {
              state = CaptureState.IDLE
              onError(writeError)
            }
          }

          val cam = CameraCapture(context)
          cam.onCameraReady = { cameraId ->
            controlChannel?.sendLensChanged(cameraId)
            pushCurrentSettings()
          }
          camera = cam
          startTimeMs = SystemClock.elapsedRealtime()
          state = CaptureState.STREAMING
          statsThread = Thread({ runStatsLoop(enc, fps, myCancelToken::get) }, "StatsLoop").apply { start() }
          cam.start(enc.inputSurface, fps, { error -> if (!myCancelToken.get()) onError(error) }, lensFacing)
        }, "VideoServer-accept")
        .apply { start() }

    // Control channel ownership moved to CaptureService: control must outlive capture
    // so the PC can wake a phone sitting in Idle (the video TCP accept above only fires when a
    // PC connects -- it can't cause itself to happen). Commands now arrive on the durable
    // channel and route into [cameraCommandListener].
  }

  fun stop() {
    // Idempotent: a write failure on the drain thread can synchronously
    // re-enter stop() (via onError) while a user-triggered call from the
    // main thread is still in progress on the same encoder/camera -- only
    // the first caller should actually tear anything down.
    if (!stopRequested.compareAndSet(false, true)) return
    cancelToken.set(true) // this cycle's own token -- see its declaration for why not `state`
    state = CaptureState.IDLE // set first: signals both accept threads this is a deliberate stop

    // Close both servers BEFORE joining either thread (not close-video/join-video/close-control/
    // join-control in sequence): the video and control accept threads are independent, so
    // unblocking them together bounds total teardown latency by whichever is slower, not the sum
    // of both. Joining sequentially previously left the abstract socket names bound for up to
    // ~2s under worst-case timing (two back-to-back 1000ms joins) -- long enough that the
    // auto-retry LaunchedEffect's rebind (500ms retry budget) reliably lost the race and the app
    // got stuck cycling Waiting-for-connection/Error{Address already in use} indefinitely,
    // confirmed live after ~35 minutes of unattended auto-retry.
    //
    // Even with that fix, a thread already blocked inside accept() isn't guaranteed to actually
    // unblock just because the socket got close()'d from here -- confirmed live: it can stay
    // blocked forever, permanently holding the abstract socket name bound so every future rebind
    // fails. That's what accept()'s own poll-with-timeout loop (VideoSocketServer) now guards
    // against -- close() here is still the fast path, but no longer the only path to actually
    // unblocking a stuck accept().
    videoServer?.close() // unblocks a pending accept(), if still waiting
    videoServer = null
    // Control channel is deliberately NOT stopped here: it's CaptureService's durable channel
    // and must keep listening (a PC StopVideo/disconnect is exactly when the next StartVideo
    // command arrives). See bindControlChannel's doc comment.

    // Guard against self-join: onError can be invoked synchronously from within this very thread
    // (the accept()-failed path), and a thread joining itself blocks for the full timeout for no
    // reason.
    if (acceptThread !== Thread.currentThread()) {
      acceptThread?.join(1000)
    }
    acceptThread = null

    camera?.stop()
    camera = null
    encoder?.stop()
    encoder = null
    // Release the session's packet sink: for the wireless transport that means closing the UDP
    // socket (the stream-backed sinks are no-ops -- their owners close the sockets). Done last,
    // after the encoder has stopped writing to it, so no packet is sent on a half-closed socket.
    videoPacketSink?.close()
    videoPacketSink = null
    startTimeMs = 0
    // Not joined -- unlike acceptThread/controlThread it holds no OS resource that must be
    // released before stop() returns, only a sleep loop that checks cancelToken every ~1s.
    statsThread = null
  }

  // Periodic telemetry (proto/control.fbs's Stats, previously defined but never sent)
  // plus a simple adaptive-bitrate policy driven by the same tick. "Dropped frames" is an
  // estimate -- MediaCodec's surface-input path doesn't expose a real drop counter -- computed as
  // expected frames at the target fps minus frames actually encoded in the interval, which is
  // exactly what "dropped" means from the pipeline's perspective regardless of which stage lost
  // them (camera, surface, or encoder).
  private fun runStatsLoop(enc: H264Encoder, targetFps: Int, isCancelled: () -> Boolean) {
    val powerManager = context.getSystemService(Context.POWER_SERVICE) as PowerManager
    var appliedBitrateBps = targetBitrateBps
    var lastFrameCount = framesEncoded
    var lastTickMs = SystemClock.elapsedRealtime()

    while (!isCancelled()) {
      Thread.sleep(1000)
      if (isCancelled()) break

      val nowMs = SystemClock.elapsedRealtime()
      val intervalSec = (nowMs - lastTickMs) / 1000.0
      val frames = framesEncoded
      val encodedThisTick = frames - lastFrameCount
      val expectedThisTick = (intervalSec * targetFps).toInt()
      val dropped = max(0, expectedThisTick - encodedThisTick)
      lastFrameCount = frames
      lastTickMs = nowMs

      val thermalStatus = mapThermalStatus(powerManager.currentThermalStatus)
      val ceiling = targetBitrateBps // re-read: onSetBitrate may have moved it since the last tick

      val newBitrate =
        when {
          thermalStatus >= ThermalStatus.Severe ->
            max(MIN_ADAPTIVE_BITRATE_BPS, (appliedBitrateBps * 0.5).toInt())
          thermalStatus == ThermalStatus.Moderate ->
            max(MIN_ADAPTIVE_BITRATE_BPS, (appliedBitrateBps * 0.75).toInt())
          thermalStatus <= ThermalStatus.Light && appliedBitrateBps < ceiling ->
            min(ceiling, (appliedBitrateBps * 1.2).toInt().coerceAtLeast(appliedBitrateBps + 1))
          else -> appliedBitrateBps
        }.coerceAtMost(ceiling)

      if (newBitrate != appliedBitrateBps) {
        appliedBitrateBps = newBitrate
        enc.setBitrate(appliedBitrateBps)
        lastBitrateBps = appliedBitrateBps
        pushCurrentSettings()
      }

      controlChannel?.sendStats(measuredFps.toFloat(), appliedBitrateBps / 1000, dropped, thermalStatus)
    }
  }

  val framesEncoded: Int
    get() = encoder?.framesEncoded ?: 0

  val elapsedSeconds: Double
    get() = if (startTimeMs == 0L) 0.0 else (SystemClock.elapsedRealtime() - startTimeMs) / 1000.0

  val measuredFps: Double
    get() = elapsedSeconds.takeIf { it > 0 }?.let { framesEncoded / it } ?: 0.0
}

// Inverse of CameraCapabilityProbe's Camera2-AfMode -> wire-AfMode mapping. Continuous always
// maps to CONTINUOUS_VIDEO (never CONTINUOUS_PICTURE) since this app only ever streams video.
// Returns null for a value the phone never advertised as supported -- callers Ack{OutOfRange}.
private fun afModeToCamera2(mode: Byte): Int? =
  when (mode) {
    AfMode.Off -> CameraMetadata.CONTROL_AF_MODE_OFF
    AfMode.Auto -> CameraMetadata.CONTROL_AF_MODE_AUTO
    AfMode.Continuous -> CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO
    AfMode.Macro -> CameraMetadata.CONTROL_AF_MODE_MACRO
    AfMode.EdgeCapture -> CameraMetadata.CONTROL_AF_MODE_EDOF
    else -> null
  }

// PowerManager's 7 thermal levels collapse onto proto/control.fbs's 5-level ThermalStatus enum --
// CRITICAL/EMERGENCY/SHUTDOWN all clamp to our top level (Critical), since by that point the
// distinction doesn't change what the PC side or the adaptive-bitrate policy should do about it.
private fun mapThermalStatus(status: Int): Byte =
  when (status) {
    PowerManager.THERMAL_STATUS_NONE -> ThermalStatus.Nominal
    PowerManager.THERMAL_STATUS_LIGHT -> ThermalStatus.Light
    PowerManager.THERMAL_STATUS_MODERATE -> ThermalStatus.Moderate
    PowerManager.THERMAL_STATUS_SEVERE -> ThermalStatus.Severe
    else -> ThermalStatus.Critical
  }
