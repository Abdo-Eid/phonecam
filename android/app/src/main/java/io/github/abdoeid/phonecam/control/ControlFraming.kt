package io.github.abdoeid.phonecam.control

import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Control-channel framing per docs/wire-protocol.md: a 4-byte little-endian length prefix
 * followed by exactly that many bytes of a FlatBuffers-encoded ControlEnvelope (proto/control.fbs).
 * The FlatBuffers buffer itself is built via the plain (non-size-prefixed) Finish -- the length
 * prefix here is the wire framing's own, not FlatBuffers' internal one; using both would nest two
 * incompatible length fields.
 */
object ControlFraming {
  fun writeEnvelope(out: OutputStream, bytes: ByteArray) {
    val header = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(bytes.size).array()
    out.write(header)
    out.write(bytes)
    out.flush()
  }

  /** Returns the envelope bytes, or throws EOFException on an orderly close mid-message. */
  fun readEnvelope(input: InputStream): ByteArray {
    val header = readExact(input, 4)
    val len = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN).int
    return readExact(input, len)
  }

  private fun readExact(input: InputStream, size: Int): ByteArray {
    val buffer = ByteArray(size)
    var total = 0
    while (total < size) {
      val n = input.read(buffer, total, size - total)
      if (n < 0) throw EOFException("control channel closed after $total/$size bytes")
      total += n
    }
    return buffer
  }
}
