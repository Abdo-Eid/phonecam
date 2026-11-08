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
import java.net.Socket

/**
 * Listens for the PC's video connection on **two** listeners at once, returning whichever one
 * connects first:
 *
 *  - the `phonecam_video` abstract socket, bridged via `adb forward tcp:27183
 *    localabstract:phonecam_video` (the USB path, needs USB debugging), and
 *  - a plain TCP listener on [tcpPort], which a PC reaches over **USB tethering**
 *    (RNDIS/NCM: the phone is the PC's gateway on that link, with no custom driver).
 *
 * Listening on both is deliberate: the PC uses either the ADB-forwarded socket or the tethered
 * network interface, depending on the selected USB connection.
 *
 * The TCP listener binds only to a detected USB-tethering interface address. It is refreshed
 * while accepting, so tethering may be enabled after the service starts. If unavailable, ADB
 * remains usable.
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
  private val tcpListener = TetheredTcpListener(tcpPort, TCP_BACKLOG, TAG)
  @Volatile private var tcpClient: Socket? = null

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
    tcpListener.open()

    var lastError: IOException? = null
    repeat(MAX_BIND_ATTEMPTS) {
      if (isCancelled()) {
        tcpListener.close()
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

    if (tcpListener.isBound) {
      Log.w(TAG, "$name: abstract socket bind failed, serving over USB-tethered TCP only", lastError)
      return
    }
    Log.w(TAG, "$name: abstract socket bind failed", lastError)
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
    if (local == null && !tcpListener.isBound) throw IOException("$name: accept() with no bound listener")

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

      if (!isCancelled()) {
        try {
          val socket = tcpListener.accept(POLL_TIMEOUT_MS) ?: continue
          // Video packets are latency-sensitive and already framed -- never let Nagle hold a
          // small tail back waiting for more data to coalesce with.
          socket.tcpNoDelay = true
          tcpClient = socket
          Log.i(TAG, "$name: accepted over TCP from ${socket.inetAddress?.hostAddress}")
          return socket.getOutputStream()
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
    tcpListener.close()
    clientSocket = null
    tcpClient = null
  }

  /** Revoke a network viewer even if capture was originally started on the phone. */
  fun closeNetworkClient() {
    try {
      tcpClient?.close()
    } catch (_: IOException) {
      // already disconnected
    }
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
