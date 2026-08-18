package io.github.abdoeid.phonecam.transport

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import java.io.IOException
import java.io.OutputStream

/**
 * Listens on the `phonecam_video` abstract socket -- bridged to the PC host via
 * `adb forward tcp:27183 localabstract:phonecam_video`, per docs/wire-protocol.md.
 *
 * [accept] polls with a short timeout and re-checks its own cancellation flag between polls,
 * rather than blocking indefinitely on the plain accept() call and depending entirely on [close]
 * to interrupt it. That used to be the whole strategy (relying on LocalServerSocket.close()
 * unblocking a concurrent blocked accept()) -- confirmed live that it isn't reliable enough:
 * observed a thread stay blocked in accept() indefinitely after close() was called from another
 * thread, permanently holding the abstract socket name bound so every future bind attempt failed
 * with "Address already in use" forever, not just during a brief race window.
 */
class VideoSocketServer(private val name: String = "phonecam_video") {
  private var serverSocket: LocalServerSocket? = null
  private var clientSocket: LocalSocket? = null

  /**
   * Binds the abstract socket, retrying briefly on "Address already in use". This is a real
   * race, not a theoretical one: a rapid Cancel-then-Start can ask to rebind the same abstract
   * name before the OS has finished releasing the just-closed previous session's socket, and an
   * unguarded bind failure here previously crashed the app (uncaught IOException out of the
   * constructor).
   *
   * [isCancelled] is polled between attempts so a concurrent [close] (this generation's own
   * teardown, not a future one's) can cut the retry loop short instead of running the full
   * budget regardless -- without it, CaptureController.stop()'s thread join can time out while
   * this loop is still retrying in the background, leaving an orphaned thread racing a
   * newly-started one for the same abstract name (confirmed live: this is what "Cancel gets
   * stuck in Address-already-in-use" turned out to be once the retry budget was widened).
   */
  fun open(isCancelled: () -> Boolean = { false }) {
    var lastError: IOException? = null
    repeat(MAX_BIND_ATTEMPTS) {
      if (isCancelled()) throw IOException("$name: open() cancelled")
      try {
        serverSocket = LocalServerSocket(name)
        return
      } catch (e: IOException) {
        lastError = e
        Thread.sleep(BIND_RETRY_DELAY_MS)
      }
    }
    throw lastError ?: IOException("failed to bind $name")
  }

  /**
   * Blocks until a client connects or [isCancelled] becomes true, polling the underlying file
   * descriptor with a short timeout in between so cancellation is noticed even if [close] doesn't
   * actually manage to interrupt an in-progress accept() (see the class doc comment).
   */
  fun accept(isCancelled: () -> Boolean = { false }): OutputStream {
    val server = serverSocket!!
    while (!isCancelled()) {
      val pollFd = StructPollfd().apply {
        fd = server.fileDescriptor
        events = OsConstants.POLLIN.toShort()
      }
      val ready =
        try {
          Os.poll(arrayOf(pollFd), POLL_TIMEOUT_MS)
        } catch (e: ErrnoException) {
          // The fd was closed out from under this poll (e.g. by a concurrent close()) -- treat
          // exactly like a cancelled accept(), not a real error.
          throw IOException("poll failed, socket likely closed", e)
        }
      if (ready > 0) {
        val socket = server.accept()
        clientSocket = socket
        return socket.outputStream
      }
    }
    throw IOException("accept() cancelled")
  }

  fun close() {
    try {
      clientSocket?.close()
    } catch (_: Exception) {
      // best-effort teardown
    }
    try {
      serverSocket?.close()
    } catch (_: Exception) {
      // best-effort teardown
    }
    clientSocket = null
  }

  private companion object {
    // 3s total budget: must safely exceed CaptureController.stop()'s worst-case teardown latency
    // (bounded by a 1000ms thread join) plus OS-level socket-release lag, or a fast
    // stop()-then-start() cycle (e.g. the auto-retry LaunchedEffect) reliably loses the bind race
    // -- confirmed live with the previous, tighter budget (10x50ms = 500ms).
    const val MAX_BIND_ATTEMPTS = 30
    const val BIND_RETRY_DELAY_MS = 100L
    const val POLL_TIMEOUT_MS = 200
  }
}
