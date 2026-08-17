package io.github.abdoeid.phonecam.capture

import android.content.Context
import android.os.SystemClock
import io.github.abdoeid.phonecam.encode.H264Encoder
import io.github.abdoeid.phonecam.transport.VideoSocketServer
import java.io.IOException
import java.util.concurrent.atomic.AtomicBoolean

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
  private val stopRequested = AtomicBoolean(false)

  @Volatile
  var state: CaptureState = CaptureState.IDLE
    private set

  fun start(width: Int, height: Int, fps: Int, bitrateBps: Int, onError: (Throwable) -> Unit) {
    stopRequested.set(false)
    state = CaptureState.WAITING_FOR_CONNECTION
    val server = VideoSocketServer()
    videoServer = server

    acceptThread =
      Thread({
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
          camera = cam
          startTimeMs = SystemClock.elapsedRealtime()
          state = CaptureState.STREAMING
          cam.start(enc.inputSurface, fps, onError)
        }, "VideoServer-accept")
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
  }

  val framesEncoded: Int
    get() = encoder?.framesEncoded ?: 0

  val elapsedSeconds: Double
    get() = if (startTimeMs == 0L) 0.0 else (SystemClock.elapsedRealtime() - startTimeMs) / 1000.0

  val measuredFps: Double
    get() = elapsedSeconds.takeIf { it > 0 }?.let { framesEncoded / it } ?: 0.0
}
