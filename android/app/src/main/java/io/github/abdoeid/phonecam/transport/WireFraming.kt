package io.github.abdoeid.phonecam.transport

import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Fixed 20-byte little-endian header + Annex-B payload, per docs/wire-protocol.md. Deliberately
 * not FlatBuffers -- this is the hot path (up to ~60 packets/sec), so it's a hand-rolled header
 * both sides implement directly with no schema evolution machinery.
 *
 * ByteBuffer defaults to BIG_ENDIAN on the JVM -- the explicit order() below is required, not
 * decorative, or the x86 PC receiver misreads seq/pts/payload_len.
 *
 * [writePacket] makes exactly one [OutputStream.write] call for the whole header+payload, not two
 * -- required for [AoaTransport]'s channel-tagging wrapper, which tags per write() call: two
 * writes per packet would have inserted a stray second tag mid-packet on that path. Harmless
 * (and marginally cheaper: one syscall instead of two) for the plain socket path too.
 */
object WireFraming {
  const val TYPE_CONFIG: Byte = 0
  const val TYPE_FRAME: Byte = 1

  private const val MAGIC: Byte = 0x9C.toByte()
  private const val FLAG_KEYFRAME: Byte = 0x01
  private const val HEADER_SIZE = 20

  fun writePacket(
    out: OutputStream,
    type: Byte,
    keyframe: Boolean,
    seq: Int,
    ptsUs: Long,
    payload: ByteArray,
    payloadOffset: Int,
    payloadLength: Int,
  ) {
    val packet =
      ByteBuffer.allocate(HEADER_SIZE + payloadLength).order(ByteOrder.LITTLE_ENDIAN).apply {
        put(MAGIC)
        put(type)
        put(if (keyframe) FLAG_KEYFRAME else 0)
        put(0) // reserved
        putInt(seq)
        putLong(ptsUs)
        putInt(payloadLength)
        put(payload, payloadOffset, payloadLength)
      }
    out.write(packet.array())
  }
}
