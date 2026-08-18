package io.github.abdoeid.phonecam.capture

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraMetadata
import android.os.SystemClock
import io.github.abdoeid.phonecam.control.ControlChannel
import io.github.abdoeid.phonecam.control.ControlCommandListener
import io.github.abdoeid.phonecam.encode.H264Encoder
import io.github.abdoeid.phonecam.transport.VideoSocketServer
import java.io.IOException
import java.util.concurrent.atomic.AtomicBoolean
import phonecam.control.AckResult
import phonecam.wire.AfMode

enum class CaptureState { IDLE, WAITING_FOR_CONNECTION, STREAMING }

/**
 * Wires CameraCapture's output straight into H264Encoder's input Surface, which streams
 * wire-framed packets (docs/wire-protocol.md) to whichever PC connects to the phonecam_video
 * socket (Phase 3). Camera and encoder don't start until a PC actually connects -- [accept]
 * blocks on a background thread -- so nothing captures or encodes into the void while no PC is
 * listening.
 */
class CaptureController(private val context: Context) {
  private var encoder: H264Encoder? = null
  private var camera: CameraCapture? = null
  private var videoServer: VideoSocketServer? = null
  private var startTimeMs: Long = 0
  private var acceptThread: Thread? = null
  private var controlChannel: ControlChannel? = null
  private var controlThread: Thread? = null
  private val stopRequested = AtomicBoolean(false)

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

  // Dispatches PC -> phone commands (docs/control-protocol.md) onto whichever camera/encoder
  // instance is live right now. Runs on ControlChannel's own receive thread; camera/encoder is
  // null until the video accept-thread finishes standing them up, and briefly null again mid
  // lens-switch -- both are real "not ready yet" states, reported as Ack{Busy} rather than
  // silently dropped or crashing.
  private val commandListener =
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

  fun start(
    width: Int,
    height: Int,
    fps: Int,
    bitrateBps: Int,
    onError: (Throwable) -> Unit,
    lensFacing: Int = CameraCharacteristics.LENS_FACING_BACK,
  ) {
    stopRequested.set(false)
    state = CaptureState.WAITING_FOR_CONNECTION
    lastWidth = width
    lastHeight = height
    lastFps = fps
    lastBitrateBps = bitrateBps
    lastZoom = 1.0f
    lastEv = 0
    lastFocusMode = AfMode.Continuous
    lastTorch = false

    acceptThread =
      Thread({
          // Binding also happens on this thread (not eagerly in start() on
          // the caller's/main thread): a rapid Cancel-then-Start can ask to
          // rebind the same abstract socket name before the OS finishes
          // releasing the just-closed previous session's socket, and
          // VideoSocketServer.open() already retries that briefly -- doing
          // it here keeps the UI thread from blocking on those retries too.
          val server = VideoSocketServer()
          try {
            server.open()
          } catch (e: IOException) {
            if (state != CaptureState.IDLE) {
              state = CaptureState.IDLE
              onError(e)
            }
            return@Thread
          }
          videoServer = server
          if (state == CaptureState.IDLE) {
            // stop() raced us during open() -- bail before accept().
            server.close()
            return@Thread
          }

          val out =
            try {
              server.accept()
            } catch (e: IOException) {
              // state == IDLE means stop() closed the socket deliberately -- not a real error.
              if (state != CaptureState.IDLE) {
                state = CaptureState.IDLE
                onError(e)
              }
              return@Thread
            }
          if (state == CaptureState.IDLE) {
            // stop() raced us right as the PC connected -- bail without starting anything.
            try {
              out.close()
            } catch (_: IOException) {
              // already torn down
            }
            return@Thread
          }

          val enc = H264Encoder(width, height, fps, bitrateBps)
          encoder = enc
          enc.start(out) { writeError ->
            state = CaptureState.IDLE
            onError(writeError)
          }

          val cam = CameraCapture(context)
          cam.onCameraReady = { cameraId ->
            controlChannel?.sendLensChanged(cameraId)
            pushCurrentSettings()
          }
          camera = cam
          startTimeMs = SystemClock.elapsedRealtime()
          state = CaptureState.STREAMING
          cam.start(enc.inputSurface, fps, onError, lensFacing)
        }, "VideoServer-accept")
        .apply { start() }

    // Control channel (Phase 4): independent socket/port, best-effort -- a
    // connect failure here (e.g. an older PC host build with no control
    // support) doesn't affect video, matching how the host side tolerates
    // this phone app predating control-channel support.
    controlThread =
      Thread({
          val channel = ControlChannel(context, commandListener)
          controlChannel = channel
          try {
            channel.start()
          } catch (_: IOException) {
            controlChannel = null
          }
        }, "ControlChannel-accept")
        .apply { start() }
  }

  fun stop() {
    // Idempotent: a write failure on the drain thread can synchronously
    // re-enter stop() (via onError) while a user-triggered call from the
    // main thread is still in progress on the same encoder/camera -- only
    // the first caller should actually tear anything down.
    if (!stopRequested.compareAndSet(false, true)) return
    state = CaptureState.IDLE // set first: signals the accept-thread this is a deliberate stop
    videoServer?.close() // unblocks a pending accept(), if still waiting
    videoServer = null
    // Guard against self-join: onError can be invoked synchronously from
    // within this very thread (the accept()-failed path), and a thread
    // joining itself blocks for the full timeout for no reason.
    if (acceptThread !== Thread.currentThread()) {
      acceptThread?.join(1000)
    }
    acceptThread = null
    camera?.stop()
    camera = null
    encoder?.stop()
    encoder = null
    startTimeMs = 0

    controlChannel?.stop() // unblocks a pending accept()/read, same as videoServer above
    controlChannel = null
    if (controlThread !== Thread.currentThread()) {
      controlThread?.join(1000)
    }
    controlThread = null
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
