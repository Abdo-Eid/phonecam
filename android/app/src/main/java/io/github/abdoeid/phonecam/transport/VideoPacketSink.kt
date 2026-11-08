package io.github.abdoeid.phonecam.transport

import java.io.IOException
import java.io.OutputStream

/**
 * Where the encoder's wire-framed packets go. This is the transport seam
 * H264Encoder/CaptureController program against: every byte that leaves the
 * encoder passes through [writePacket], keeping the encoder independent of
 * socket setup and lifecycle.
 */
interface VideoPacketSink {
  /**
   * Delivers one complete wire-protocol packet (see docs/wire-protocol.md's
   * framing table). Exactly the same shape [WireFraming.writePacket] encodes,
   * so the OutputStream-backed implementations are 1:1 with it.
   *
   * @throws IOException on a broken destination -- propagates to the encoder's
   *   drain thread, which stops and reports through the standard onWriteError
   *   teardown path (H264Encoder documents why it must not be swallowed).
   */
  @Throws(IOException::class)
  fun writePacket(
    type: Byte,
    keyframe: Boolean,
    seq: Int,
    ptsUs: Long,
    payload: ByteArray,
    payloadOffset: Int,
    payloadLength: Int,
  )

  /**
   * Releases resources owned by the sink. The stream's socket owner
   * ([VideoSocketServer]) performs socket teardown.
   */
  fun close() {}
}

/**
 * Adapter for ADB abstract sockets and tethered TCP via [VideoSocketServer.accept].
 * Delegates to [WireFraming].
 */
class OutputStreamVideoSink(private val out: OutputStream) : VideoPacketSink {
  override fun writePacket(
    type: Byte,
    keyframe: Boolean,
    seq: Int,
    ptsUs: Long,
    payload: ByteArray,
    payloadOffset: Int,
    payloadLength: Int,
  ) {
    WireFraming.writePacket(out, type, keyframe, seq, ptsUs, payload, payloadOffset, payloadLength)
  }
}
