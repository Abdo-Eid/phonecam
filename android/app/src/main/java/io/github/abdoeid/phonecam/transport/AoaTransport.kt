package io.github.abdoeid.phonecam.transport

import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.ParcelFileDescriptor
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream
import phonecam.wire.Channel

/**
 * Phase 7 production AOA transport: multiplexes the video and control channels
 * (`proto/wire.fbs`'s `Channel` enum) over the single pair of bulk endpoints an AOA accessory
 * connection provides, per docs/wire-protocol.md's "AOA transport" section. Distinct from
 * [AoaAccessoryTransport], which stays as the Phase-7-checkpoint-only diagnostic (see its own doc
 * comment) -- this is the real thing [io.github.abdoeid.phonecam.capture.CaptureController] uses
 * once wired in.
 *
 * Framing: each logical message is a single OS-level [FileOutputStream.write] of
 * `[1-byte Channel tag][self-delimiting body]`. The two channels' bodies are self-delimiting
 * differently, and the tag write must match: video bodies are [WireFraming]'s own
 * 20-byte-header-plus-payload, which already carries its own `payload_len` field, so
 * [videoOutput] prepends *only* the 1-byte tag -- an earlier version of this class also added a
 * 4-byte length prefix here, which corrupted every packet after the first (confirmed live: only
 * one CONFIG packet, zero FRAMEs, ever parsed correctly on the host side) since the host's parser
 * (rightly) doesn't expect one. Control bodies (raw FlatBuffers bytes, not self-delimiting on
 * their own) get `[tag][4-byte LE length][envelope]`. [WireFraming.writePacket] makes exactly one
 * write() call per packet specifically so [videoOutput] can tag each write() atomically -- see
 * its own doc comment.
 *
 * The reader must not assume the tag and body arrive in the same read() call or the same USB
 * transfer. [AoaAccessoryTransport]'s doc comment records the concrete bug (and fix) this project
 * already hit getting that wrong: a read() smaller than a queued USB transfer only consumes part
 * of it, permanently losing the rest -- reads here are always sized to a generous fixed buffer,
 * never smaller, and parsed from an accumulator that carries over incomplete trailing bytes.
 */
class AoaTransport {
  private var fd: ParcelFileDescriptor? = null
  private var output: FileOutputStream? = null
  private var input: FileInputStream? = null
  private var readerThread: Thread? = null
  @Volatile private var running = false

  // Concurrent writers: the video-encoder thread (via [videoOutput]) and, later, whatever thread
  // sends control replies -- both share one FileOutputStream, guarded so a tag+body pair from one
  // channel can never land interleaved with another's.
  private val outputLock = Any()

  /** Invoked on the reader thread for each complete control envelope received from the PC. */
  var onControlMessage: ((ByteArray) -> Unit)? = null

  val isOpen: Boolean
    get() = fd != null

  /**
   * Tags every write() as the video channel. Each write() call must already be one complete
   * logical packet -- see class doc for why [WireFraming.writePacket] guarantees that.
   */
  val videoOutput: OutputStream = TaggedOutputStream()

  fun open(usbManager: UsbManager, accessory: UsbAccessory): Boolean {
    val opened =
      try {
        usbManager.openAccessory(accessory)
      } catch (e: IllegalArgumentException) {
        // openAccessory() throws instead of returning null if the accessory reference is no
        // longer valid (e.g. already unplugged since the intent fired) -- treated the same as a
        // null return, not a crash.
        null
      }
    if (opened == null) return false

    fd = opened
    output = FileOutputStream(opened.fileDescriptor)
    input = FileInputStream(opened.fileDescriptor)
    running = true
    readerThread = Thread(::runReader, "AoaTransportReader").apply { start() }
    return true
  }

  /** @throws IOException if the pipe is broken -- propagates, does not swallow. See class doc:
   *  a silently-swallowed write failure here previously meant nothing ever noticed a broken
   *  pipe, so the encoder kept "streaming" into a dead connection indefinitely instead of
   *  hitting its existing onWriteError teardown path. */
  fun writeControlMessage(envelope: ByteArray) {
    val header = ByteArray(5)
    header[0] = Channel.Control.toByte()
    writeLeU32(header, 1, envelope.size)
    writeRaw(header, envelope, 0, envelope.size)
  }

  /** Tags [body] as the video channel -- no length prefix, see class doc for why. */
  private fun writeVideoTagged(body: ByteArray, offset: Int, length: Int) {
    writeRaw(byteArrayOf(Channel.Video.toByte()), body, offset, length)
  }

  private fun writeLeU32(buf: ByteArray, offset: Int, value: Int) {
    buf[offset] = (value and 0xFF).toByte()
    buf[offset + 1] = ((value ushr 8) and 0xFF).toByte()
    buf[offset + 2] = ((value ushr 16) and 0xFF).toByte()
    buf[offset + 3] = ((value ushr 24) and 0xFF).toByte()
  }

  /** @throws IOException if the pipe is broken -- see [writeControlMessage]'s doc comment. */
  private fun writeRaw(header: ByteArray, body: ByteArray, bodyOffset: Int, bodyLength: Int) {
    synchronized(outputLock) {
      output?.write(header)
      output?.write(body, bodyOffset, bodyLength)
      output?.flush()
    }
  }

  private inner class TaggedOutputStream : OutputStream() {
    override fun write(b: Int) = write(byteArrayOf(b.toByte()), 0, 1)

    override fun write(b: ByteArray, off: Int, len: Int) = writeVideoTagged(b, off, len)
  }

  // Accumulator buffer for [runReader] -- grows via copyOf only when genuinely out of room, and
  // otherwise reused by shifting unconsumed trailing bytes to the front each read, avoiding a
  // fresh allocation on every read() call (relevant at sustained video rates: ~30 reads/sec,
  // not just this test's one-shot bursts).
  private var accum = ByteArray(64 * 1024)
  private var accumLen = 0

  private fun runReader() {
    val stream = input ?: return
    val readTarget = ByteArray(16 * 1024)
    while (running) {
      try {
        val n = stream.read(readTarget)
        if (n <= 0) break
        if (accum.size < accumLen + n) {
          var newCap = accum.size * 2
          while (newCap < accumLen + n) newCap *= 2
          accum = accum.copyOf(newCap)
        }
        System.arraycopy(readTarget, 0, accum, accumLen, n)
        accumLen += n
        val consumed = consumeFrames(accum, accumLen)
        if (consumed > 0) {
          System.arraycopy(accum, consumed, accum, 0, accumLen - consumed)
          accumLen -= consumed
        }
      } catch (e: IOException) {
        break
      }
    }
  }

  /** Dispatches every complete frame at the front of `buf[0, len)`; returns bytes consumed. */
  private fun consumeFrames(buf: ByteArray, len: Int): Int {
    var pos = 0
    while (pos < len) {
      // Key on the tag first, then interpret the body -- never scan for a body-internal byte
      // pattern (e.g. WireFraming's magic) to resync, which could false-lock onto payload data.
      val tag = buf[pos].toUByte()
      if (tag != Channel.Control) {
        // Video (phone->PC only) or an unexpected tag arriving inbound -- not a frame shape this
        // side can parse a length out of. Drop one byte and resync on the next rather than
        // getting stuck; this is not expected to happen in normal operation.
        pos += 1
        continue
      }
      if (len - pos < 5) break // length prefix not fully arrived yet
      val bodyLen = readLeU32(buf, pos + 1)
      if (len - pos < 5 + bodyLen) break // body not fully arrived yet
      val envelope = buf.copyOfRange(pos + 5, pos + 5 + bodyLen)
      onControlMessage?.invoke(envelope)
      pos += 5 + bodyLen
    }
    return pos
  }

  private fun readLeU32(buf: ByteArray, offset: Int): Int =
    (buf[offset].toInt() and 0xFF) or
      ((buf[offset + 1].toInt() and 0xFF) shl 8) or
      ((buf[offset + 2].toInt() and 0xFF) shl 16) or
      ((buf[offset + 3].toInt() and 0xFF) shl 24)

  fun close() {
    running = false
    readerThread?.interrupt()
    readerThread = null
    try {
      output?.close()
    } catch (e: IOException) {
      // Closing an already-broken pipe -- nothing to recover, this is teardown.
    }
    try {
      input?.close()
    } catch (e: IOException) {
      // Same as above.
    }
    try {
      fd?.close()
    } catch (e: IOException) {
      // Same as above.
    }
    output = null
    input = null
    fd = null
  }
}
