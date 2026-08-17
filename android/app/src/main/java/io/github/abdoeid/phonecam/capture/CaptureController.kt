package io.github.abdoeid.phonecam.capture

import android.content.Context
import android.os.SystemClock
import io.github.abdoeid.phonecam.encode.H264Encoder
import java.io.File
import java.io.FileOutputStream

/** Wires CameraCapture's output straight into H264Encoder's input Surface and owns both
 * lifecycles plus the output file for the Phase 2 "on-device .h264 plays back" proof. */
class CaptureController(private val context: Context) {
  private var encoder: H264Encoder? = null
  private var camera: CameraCapture? = null
  private var outputStream: FileOutputStream? = null
  private var startTimeMs: Long = 0

  var outputFile: File? = null
    private set

  fun start(width: Int, height: Int, fps: Int, bitrateBps: Int, onError: (Throwable) -> Unit) {
    val file = File(context.getExternalFilesDir(null), "capture_${System.currentTimeMillis()}.h264")
    outputFile = file
    val stream = FileOutputStream(file)
    outputStream = stream

    val enc = H264Encoder(width, height, fps, bitrateBps)
    encoder = enc
    enc.start(stream)

    val cam = CameraCapture(context)
    camera = cam
    startTimeMs = SystemClock.elapsedRealtime()
    cam.start(enc.inputSurface, fps, onError)
  }

  fun stop() {
    camera?.stop()
    camera = null
    encoder?.stop()
    encoder = null
    outputStream?.close()
    outputStream = null
    startTimeMs = 0
  }

  val framesEncoded: Int
    get() = encoder?.framesEncoded ?: 0

  val elapsedSeconds: Double
    get() = if (startTimeMs == 0L) 0.0 else (SystemClock.elapsedRealtime() - startTimeMs) / 1000.0

  val measuredFps: Double
    get() = elapsedSeconds.takeIf { it > 0 }?.let { framesEncoded / it } ?: 0.0
}
