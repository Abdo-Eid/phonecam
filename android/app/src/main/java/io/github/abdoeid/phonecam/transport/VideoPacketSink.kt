package io.github.abdoeid.phonecam.transport

import java.io.IOException
import java.io.OutputStream

/**
 * Where the encoder's wire-framed packets go. This is the transport seam
 * H264Encoder/CaptureController program against: every byte that leaves the
 * encoder passes through [writePacket], so a new transport only needs to
 * implement this interface -- no encoder/controller change (that decoupling is
 * what let the AOA transport and now the RTP transport
 * work: the AOA path tags one write() call as the video channel, RTP turns
 * each framed packet into one or more UDP datagrams).
 *
 * Packet identity (type/keyframe/seq/pts) travels as arguments, not hidden
 * inside a byte buffer -- [RtpVideoSink] needs the timestamps and keyframe
 * info itself, and an OutputStream-based sink would otherwise have to parse
 * the 20-byte header back out of its own bytes to recover them.
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
   * Releases this sink's own resources (e.g. an open UDP socket). Called by
   * CaptureController.stop() on teardown. Default no-op: stream-backed sinks
   * must NOT close the underlying stream here -- the socket's owner
   * (VideoSocketServer / AoaTransport) tears those down itself, and closing an
   * owner-held stream from two directions is exactly the reentrancy shape this
   * project has already been bitten by twice (see CaptureController.stop()'s
   * comments).
   */
  fun close() {}
}

/**
 * Adapter for transports whose packets already travel as OutputStream writes:
 * USB (abstract socket / TCP via VideoSocketServer.accept) and the AOA
 * accessory pipe. Delegates to [WireFraming], preserving that contract exactly
 * (including the single-write-per-packet guarantee AoaTransport's
 * channel-tagging wrapper relies on).
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
