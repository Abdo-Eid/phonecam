package io.github.abdoeid.phonecam.encode

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.view.Surface
import io.github.abdoeid.phonecam.transport.WireFraming
import java.io.IOException
import java.io.OutputStream
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/**
 * Hardware H.264 encoder using a MediaCodec input Surface (Camera2 renders directly into it --
 * zero-copy, no CPU frame ever exists). Output is Annex-B (start-code-prefixed NALUs, including
 * CSD/SPS/PPS), written to [out] as wire-framed CONFIG/FRAME packets per docs/wire-protocol.md
 * (see [WireFraming]) -- [out] is the connected video socket's OutputStream (Phase 3).
 *
 * Constrained-baseline / CBR / no B-frames / low-latency, per docs/architecture.md Phase 2.
 */
class H264Encoder(width: Int, height: Int, fps: Int, bitrateBps: Int) {
  val inputSurface: Surface

  private val codec: MediaCodec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
  private var drainThread: Thread? = null
  private val running = AtomicBoolean(false)
  private val frameCount = AtomicInteger(0)
  private var seq = 0

  // One instance is used for exactly one start()/stop() cycle (CaptureController always
  // constructs a fresh H264Encoder per capture session), so a single-shot guard is enough.
  private val stopRequested = AtomicBoolean(false)

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

  /**
   * Starts the encoder and a background thread draining wire-framed output into [out]. [onWriteError]
   * fires once (from the drain thread) if a socket write fails -- e.g. the PC host disconnected --
   * at which point the drain loop stops draining; the caller owns deciding what happens next
   * (Phase 3 keeps this simple: stop capture, surface an error, no auto-reconnect -- that's Phase 5).
   */
  fun start(out: OutputStream, onWriteError: (IOException) -> Unit) {
    codec.start()
    running.set(true)
    drainThread =
      Thread({ drainLoop(out, onWriteError) }, "H264Encoder-drain").apply { start() }
  }

  fun stop() {
    // Idempotent: onWriteError can synchronously re-enter via
    // CaptureController.stop() -> encoder.stop() while a concurrent caller
    // (e.g. the user-triggered stop on the main thread) is still in the
    // middle of its own call to this same method -- only the first caller
    // should touch codec.
    if (!stopRequested.compareAndSet(false, true)) return
    running.set(false)
    // Guard against self-join: onWriteError can be invoked synchronously
    // from within this very thread, and a thread joining itself blocks for
    // the full timeout for no reason.
    if (drainThread !== Thread.currentThread()) {
      drainThread?.join(1000)
    }
    drainThread = null
    try {
      codec.stop()
    } catch (_: IllegalStateException) {
      // already stopped/never started producing output -- fine to ignore for this dev harness
    }
    codec.release()
    inputSurface.release()
  }

  private fun drainLoop(out: OutputStream, onWriteError: (IOException) -> Unit) {
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

        val isConfig = bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
        val isKeyframe = isConfig || (bufferInfo.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0)
        try {
          WireFraming.writePacket(
            out,
            if (isConfig) WireFraming.TYPE_CONFIG else WireFraming.TYPE_FRAME,
            isKeyframe,
            seq++,
            bufferInfo.presentationTimeUs,
            bytes,
            0,
            bytes.size,
          )
        } catch (e: IOException) {
          running.set(false)
          // Don't touch codec after this: onWriteError can synchronously
          // chain into CaptureController.stop() -> encoder.stop(), which
          // releases codec (the self-join guard there correctly recognizes
          // we're already on this thread and skips joining, but the codec
          // release still happens before this call returns) -- calling
          // releaseOutputBuffer afterward would hit an already-released
          // codec and throw IllegalStateException.
          onWriteError(e)
          return
        }
        if (!isConfig) {
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
