package io.github.abdoeid.phonecam.encode

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.view.Surface
import java.io.OutputStream
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/**
 * Hardware H.264 encoder using a MediaCodec input Surface (Camera2 renders directly into it --
 * zero-copy, no CPU frame ever exists). Output is Annex-B (start-code-prefixed NALUs, including
 * CSD/SPS/PPS), matching docs/wire-protocol.md's video framing so this can feed the wire protocol
 * directly once the USB transport (Phase 3) exists. For now it just writes the raw elementary
 * stream to a file for the Phase 2 "on-device .h264 plays back" proof.
 *
 * Constrained-baseline / CBR / no B-frames / low-latency, per docs/architecture.md Phase 2.
 */
class H264Encoder(width: Int, height: Int, fps: Int, bitrateBps: Int) {
  val inputSurface: Surface

  private val codec: MediaCodec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
  private var drainThread: Thread? = null
  private val running = AtomicBoolean(false)
  private val frameCount = AtomicInteger(0)

  val framesEncoded: Int
    get() = frameCount.get()

  init {
    // Deliberately minimal for now: the SD665's vendor AVC encoder rejected
    // configure() (CodecException 0x80001001, "unsupported setting") with
    // KEY_PROFILE/KEY_MAX_B_FRAMES/KEY_LATENCY/KEY_PRIORITY set -- likely one
    // of the API-29+ keys (KEY_MAX_B_FRAMES/KEY_LATENCY) isn't honored by this
    // chip's HAL, or KEY_PROFILE needs a paired KEY_LEVEL (a known Qualcomm
    // quirk). Get this baseline working, then reintroduce the low-latency/
    // constrained-baseline knobs one at a time against the real device.
    val format =
      MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
        setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        setInteger(MediaFormat.KEY_BIT_RATE, bitrateBps)
        setInteger(MediaFormat.KEY_FRAME_RATE, fps)
        setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
      }
    codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
    inputSurface = codec.createInputSurface()
  }

  /** Starts the encoder and a background thread draining encoded Annex-B output into [out]. */
  fun start(out: OutputStream) {
    codec.start()
    running.set(true)
    drainThread =
      Thread({ drainLoop(out) }, "H264Encoder-drain").apply { start() }
  }

  fun stop() {
    running.set(false)
    drainThread?.join(1000)
    drainThread = null
    try {
      codec.stop()
    } catch (_: IllegalStateException) {
      // already stopped/never started producing output -- fine to ignore for this dev harness
    }
    codec.release()
    inputSurface.release()
  }

  private fun drainLoop(out: OutputStream) {
    val bufferInfo = MediaCodec.BufferInfo()
    while (running.get()) {
      val index = codec.dequeueOutputBuffer(bufferInfo, DEQUEUE_TIMEOUT_US)
      if (index < 0) continue // INFO_TRY_AGAIN_LATER or a format/buffers-changed event -- nothing to write

      val buffer = codec.getOutputBuffer(index) ?: continue
      if (bufferInfo.size > 0) {
        buffer.position(bufferInfo.offset)
        buffer.limit(bufferInfo.offset + bufferInfo.size)
        val bytes = ByteArray(bufferInfo.size)
        buffer.get(bytes)
        out.write(bytes)
        if (bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {
          frameCount.incrementAndGet()
        }
      }
      codec.releaseOutputBuffer(index, false)
    }
  }

  private companion object {
    const val DEQUEUE_TIMEOUT_US = 10_000L
  }
}
