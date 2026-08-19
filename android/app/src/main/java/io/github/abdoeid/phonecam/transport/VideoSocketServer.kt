package io.github.abdoeid.phonecam.transport

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import android.util.Log
import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException

/**
 * Listens for the PC's video connection on **two** listeners at once, returning whichever one
 * connects first:
 *
 *  - the `phonecam_video` abstract socket, bridged via `adb forward tcp:27183
 *    localabstract:phonecam_video` (the Phase 3 path, needs USB debugging), and
 *  - a plain TCP listener on [tcpPort] (Phase 8), which a PC reaches directly over **USB
 *    tethering** (RNDIS: the phone is the PC's default gateway on that link, ~3-5ms round trip)
 *    or over Wi-Fi -- no adb, no Developer Options, and no Windows driver install at all, since
 *    Windows binds its own inbox RNDIS driver automatically.
 *
 * Listening on both is deliberate: it means the user never picks a mode. Whichever way the PC
 * can reach the phone, it just works, and the app needs no transport toggle in its UI.
 *
 * The TCP bind is best-effort -- if it fails (port busy, permission), the abstract socket alone
 * still works exactly as before, and vice versa. Only failing *both* is fatal.
 *
 * [accept] polls with a short timeout and re-checks its own cancellation flag between polls,
 * rather than blocking indefinitely on the plain accept() call and depending entirely on [close]
 * to interrupt it. That used to be the whole strategy (relying on LocalServerSocket.close()
 * unblocking a concurrent blocked accept()) -- confirmed live that it isn't reliable enough:
 * observed a thread stay blocked in accept() indefinitely after close() was called from another
 * thread, permanently holding the abstract socket name bound so every future bind attempt failed
 * with "Address already in use" forever, not just during a brief race window.
 */
class VideoSocketServer(
  private val name: String = "phonecam_video",
  private val tcpPort: Int = DEFAULT_TCP_PORT,
) {
  private var serverSocket: LocalServerSocket? = null
  private var clientSocket: LocalSocket? = null
  private var tcpServer: ServerSocket? = null
  private var tcpClient: Socket? = null

  /**
   * Binds both listeners. The abstract socket retries briefly on "Address already in use" -- a
   * real race, not a theoretical one: a rapid Cancel-then-Start can ask to rebind the same
   * abstract name before the OS has finished releasing the just-closed previous session's socket,
   * and an unguarded bind failure here previously crashed the app (uncaught IOException out of
   * the constructor).
   *
   * [isCancelled] is polled between attempts so a concurrent [close] (this generation's own
   * teardown, not a future one's) can cut the retry loop short instead of running the full
   * budget regardless -- without it, CaptureController.stop()'s thread join can time out while
   * this loop is still retrying in the background, leaving an orphaned thread racing a
   * newly-started one for the same abstract name (confirmed live: this is what "Cancel gets
   * stuck in Address-already-in-use" turned out to be once the retry budget was widened).
   *
   * Throws only if BOTH listeners fail to bind -- one working listener is enough to serve a PC.
   */
  fun open(isCancelled: () -> Boolean = { false }) {
    // TCP first: it binds instantly, so it's ready to serve even if the abstract socket below
    // spends its full retry budget losing a rebind race.
    tcpServer =
      try {
        ServerSocket().apply {
          reuseAddress = true
          bind(InetSocketAddress(tcpPort), TCP_BACKLOG)
          soTimeout = POLL_TIMEOUT_MS
        }
      } catch (e: IOException) {
        Log.w(TAG, "$name: TCP bind on port $tcpPort failed, abstract socket only", e)
        null
      }

    var lastError: IOException? = null
    repeat(MAX_BIND_ATTEMPTS) {
      if (isCancelled()) {
        closeTcpServer()
        throw IOException("$name: open() cancelled")
      }
      try {
        serverSocket = LocalServerSocket(name)
        return
      } catch (e: IOException) {
        lastError = e
        Thread.sleep(BIND_RETRY_DELAY_MS)
      }
    }

    if (tcpServer != null) {
      Log.w(TAG, "$name: abstract socket bind failed, serving over TCP only", lastError)
      return
    }
    throw lastError ?: IOException("failed to bind $name")
  }

  /**
   * Blocks until a client connects on either listener or [isCancelled] becomes true. Each
   * listener is polled with a short timeout in turn, so cancellation is noticed even if [close]
   * doesn't actually manage to interrupt an in-progress accept() (see the class doc comment),
   * and neither listener can starve the other.
   *
   * A connect is never actually *delayed* by this alternation: the PC's connect() completes at
   * the OS level as soon as the listener exists (TCP backlog / abstract-socket queue), so the
   * poll cadence only affects how quickly this thread notices, not the PC's connect latency.
   */
  fun accept(isCancelled: () -> Boolean = { false }): OutputStream {
    val local = serverSocket
    val tcp = tcpServer
    if (local == null && tcp == null) throw IOException("$name: accept() with no bound listener")

    while (!isCancelled()) {
      if (local != null) {
        val pollFd = StructPollfd().apply {
          fd = local.fileDescriptor
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
          val socket = local.accept()
          clientSocket = socket
          Log.i(TAG, "$name: accepted over the abstract socket (adb)")
          return socket.outputStream
        }
      }

      if (tcp != null) {
        try {
          val socket = tcp.accept()
          // Video packets are latency-sensitive and already framed -- never let Nagle hold a
          // small tail back waiting for more data to coalesce with.
          socket.tcpNoDelay = true
          tcpClient = socket
          Log.i(TAG, "$name: accepted over TCP from ${socket.inetAddress?.hostAddress}")
          return socket.getOutputStream()
        } catch (_: SocketTimeoutException) {
          // Normal: nothing connected within soTimeout, fall through and re-check cancellation.
        } catch (e: IOException) {
          if (!isCancelled()) throw e  // a real error, not close() racing us during teardown
        }
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
    try {
      tcpClient?.close()
    } catch (_: Exception) {
      // best-effort teardown
    }
    closeTcpServer()
    clientSocket = null
    tcpClient = null
  }

  private fun closeTcpServer() {
    try {
      tcpServer?.close()
    } catch (_: Exception) {
      // best-effort teardown
    }
    tcpServer = null
  }

  private companion object {
    const val TAG = "VideoSocketServer"

    // Must match windows/host/transport/AdbTransport.cpp's kPort and NetTransport.cpp's
    // kVideoPort -- the same port number serves both the adb-forwarded and the direct-TCP path,
    // since adb forward's PC-side port is just a local alias for this one.
    const val DEFAULT_TCP_PORT = 27183
    const val TCP_BACKLOG = 4

    // 3s total budget: must safely exceed CaptureController.stop()'s worst-case teardown latency
    // (bounded by a 1000ms thread join) plus OS-level socket-release lag, or a fast
    // stop()-then-start() cycle (e.g. the auto-retry LaunchedEffect) reliably loses the bind race
    // -- confirmed live with the previous, tighter budget (10x50ms = 500ms).
    const val MAX_BIND_ATTEMPTS = 30
    const val BIND_RETRY_DELAY_MS = 100L
    const val POLL_TIMEOUT_MS = 200
  }
}
