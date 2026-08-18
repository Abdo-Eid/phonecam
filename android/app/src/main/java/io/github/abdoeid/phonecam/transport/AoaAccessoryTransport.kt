package io.github.abdoeid.phonecam.transport

import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.ParcelFileDescriptor
import android.util.Log
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException

private const val TAG = "PhoneCamAoa"

/**
 * Phase 7 AOA transport, seeded from a single-direction pipe checkpoint before any multiplexing
 * exists (see docs/wire-protocol.md's "AOA transport" section). [openAndSendCheckpoint] proves
 * openAccessory() actually yields a live, host-readable bulk pipe -- nothing more. The real
 * video/control multiplexing (per-message 1-byte Channel tag, see proto/wire.fbs) is later work
 * on top of the [FileOutputStream]/[FileInputStream] this opens.
 *
 * The checkpoint write is a repeating heartbeat (every 500ms), not a single write. A one-shot
 * write turned host-side verification into a race that had to catch one specific ~ms-wide window
 * -- indistinguishable, from the host, between "the pipe doesn't work" and "the host just wasn't
 * polling at the right instant." A heartbeat makes that ambiguity go away: if the host is ever
 * going to see data, it'll see it within one heartbeat interval, not "possibly, if the timing
 * lined up."
 *
 * [runReader] is a second, throwaway rehearsal: it verifies whatever `windows/tools/aoa_probe`'s
 * OUT-direction tests send (a short text marker, then a larger length-prefixed payload) and
 * writes an ACK line back over the same pipe, so that direction and multi-packet reassembly can
 * be proven end-to-end over USB alone -- `adb logcat` turned out to be unusable for this
 * verification (MIUI silently drops this app's own Log.* output entirely while it's backgrounded
 * holding the accessory open; confirmed by checking every tag and the whole process's PID with
 * zero app-generated lines, not just this one). Not the real wire-protocol.md Channel framing --
 * just enough to prove the pipe and the reassembly logic both work.
 */
class AoaAccessoryTransport {
  private var fd: ParcelFileDescriptor? = null
  private var output: FileOutputStream? = null
  private var input: FileInputStream? = null
  private var heartbeatThread: Thread? = null
  private var readerThread: Thread? = null
  @Volatile private var running = false
  // Heartbeat and ACK writes share one FileOutputStream on independent threads -- guards against
  // interleaved partial writes landing in the same bulk OUT transfer.
  private val outputLock = Any()

  val isOpen: Boolean
    get() = fd != null

  /** Opens the accessory and starts the heartbeat write loop. See class doc for why it repeats. */
  fun openAndSendCheckpoint(usbManager: UsbManager, accessory: UsbAccessory): Boolean {
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
    heartbeatThread = Thread(::runHeartbeat, "AoaCheckpointHeartbeat").apply { start() }
    readerThread = Thread(::runReader, "AoaOutTestReader").apply { start() }
    return true
  }

  private fun runHeartbeat() {
    while (running) {
      try {
        synchronized(outputLock) {
          output?.write(CHECKPOINT_PATTERN)
          output?.flush()
        }
      } catch (e: IOException) {
        break
      }
      try {
        Thread.sleep(500)
      } catch (e: InterruptedException) {
        break
      }
    }
  }

  private fun writeAck(text: String) {
    try {
      synchronized(outputLock) {
        output?.write((text + "\n").toByteArray(Charsets.US_ASCII))
        output?.flush()
      }
    } catch (e: IOException) {
      // Ack is best-effort diagnostics -- nothing to recover if the pipe is already broken.
    }
  }

  // Real finding, not a guess: an earlier version of this reader did 1-byte-at-a-time
  // stream.read() calls to find framing boundaries (tag byte, then a byte-by-byte scan for '\n').
  // That stalled the whole pipe -- confirmed live: the host received an immediate ACK for the tag
  // byte of a 24-byte marker frame, then nothing further, and the host's *next* write (a separate,
  // unrelated 128KB frame) then timed out after 10s trying to write to the OUT endpoint at all.
  // Explanation: the accessory pipe is backed by a raw USB transfer queue, not a buffered stream --
  // a read() smaller than a queued transfer only consumes part of it, and the remainder is not
  // retained for the next read() call to pick up. A 1-byte read draining a 24-byte transfer left
  // 23 bytes permanently lost, desyncing the parser (stuck waiting for a '\n' that already went by)
  // and leaving the endpoint's queue in a state that never drained, which is what stalled the
  // host's next write. Fix: never read smaller than the largest possible incoming chunk -- always
  // read into a generously-sized buffer and parse complete frames out of an in-memory accumulator,
  // carrying over only genuinely incomplete trailing bytes to the next read.
  private fun runReader() {
    val stream = input ?: return
    val chunk = ByteArray(16 * 1024)
    var pending = ByteArray(0)
    while (running) {
      try {
        val n = stream.read(chunk)
        if (n <= 0) break
        pending = pending + chunk.copyOf(n)
        pending = consumeFrames(pending)
      } catch (e: IOException) {
        break
      } catch (e: Exception) {
        // Diagnostics-only widening: Log is unusable here (MIUI drops it while backgrounded, see
        // class doc), so report any unexpected exception the same way as a successful ACK rather
        // than silently killing this thread with no visible trace at all.
        writeAck("ACK:reader-exception:${e.javaClass.simpleName}:${e.message}")
        break
      }
    }
  }

  /** Extracts and acks every complete frame at the front of [buf]; returns the incomplete tail. */
  private fun consumeFrames(bufIn: ByteArray): ByteArray {
    var buf = bufIn
    while (buf.isNotEmpty()) {
      when (buf[0].toInt()) {
        0 -> {
          val newlineIdx = buf.indexOf('\n'.code.toByte())
          if (newlineIdx < 0) return buf  // marker not fully arrived yet
          val text = String(buf, 1, newlineIdx - 1, Charsets.US_ASCII)
          Log.i(TAG, "OUT-test text marker received: \"$text\"")
          writeAck("ACK:text:len=${text.length}")
          buf = buf.copyOfRange(newlineIdx + 1, buf.size)
        }
        1 -> {
          if (buf.size < 5) return buf  // length prefix not fully arrived yet
          val length =
            (buf[1].toInt() and 0xFF) or
              ((buf[2].toInt() and 0xFF) shl 8) or
              ((buf[3].toInt() and 0xFF) shl 16) or
              ((buf[4].toInt() and 0xFF) shl 24)
          if (buf.size < 5 + length) return buf  // payload not fully arrived yet
          var mismatchAt = -1
          for (i in 0 until length) {
            if (buf[5 + i] != (i and 0xFF).toByte()) {
              mismatchAt = i
              break
            }
          }
          if (mismatchAt < 0) {
            Log.i(TAG, "OUT-test length-prefixed: received $length bytes, pattern OK")
            writeAck("ACK:bin:len=$length:ok=1")
          } else {
            Log.e(TAG, "OUT-test length-prefixed: received $length bytes, MISMATCH at byte $mismatchAt")
            writeAck("ACK:bin:len=$length:ok=0:mismatchAt=$mismatchAt")
          }
          buf = buf.copyOfRange(5 + length, buf.size)
        }
        else -> {
          writeAck("ACK:unknown-tag=${buf[0]}")
          return ByteArray(0)  // diagnostics-only: no recovery framing to resync on
        }
      }
    }
    return buf
  }

  fun close() {
    running = false
    heartbeatThread?.interrupt()
    heartbeatThread = null
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

  companion object {
    // Plain ASCII, not part of the real wire protocol -- just something a throwaway host-side
    // reader can recognize unambiguously in a hex/text dump.
    val CHECKPOINT_PATTERN: ByteArray = "PHONECAM-AOA-PIPE-OK\n".toByteArray(Charsets.US_ASCII)
  }
}
