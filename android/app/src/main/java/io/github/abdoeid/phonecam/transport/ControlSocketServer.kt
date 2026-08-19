package io.github.abdoeid.phonecam.transport

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import android.util.Log
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException

/**
 * Listens for the PC's control connection on the `phonecam_control` abstract socket (bridged via
 * `adb forward tcp:27184 localabstract:phonecam_control`) and on a plain TCP listener at
 * [tcpPort], returning whichever connects first -- same dual-listener shape as
 * [VideoSocketServer], see its class doc for the full reasoning.
 *
 * Bidirectional (unlike [VideoSocketServer]): the PC sends commands, the phone sends
 * capabilities/telemetry back on the same connection.
 *
 * The TCP listener is what makes the camera controls (lens/zoom/exposure/torch/focus) work
 * without USB debugging at all -- over AOA they were unreachable, because AOA only ever carried
 * video and this channel had no route to the phone. Over USB tethering both channels are just
 * TCP to the same device, so control comes back for free.
 */
class ControlSocketServer(
  private val name: String = "phonecam_control",
  private val tcpPort: Int = DEFAULT_TCP_PORT,
) {
  private var serverSocket: LocalServerSocket? = null
  private var clientSocket: LocalSocket? = null
  private var tcpServer: ServerSocket? = null
  private var tcpClient: Socket? = null

  // isCancelled / retry budget / both-listeners rationale: see VideoSocketServer.open()'s doc
  // comment -- same reasoning, same fix, same best-effort TCP bind.
  fun open(isCancelled: () -> Boolean = { false }) {
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

  // isCancelled / alternating-poll rationale: see VideoSocketServer.accept()'s doc comment.
  fun accept(isCancelled: () -> Boolean = { false }): Pair<InputStream, OutputStream> {
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
            throw IOException("poll failed, socket likely closed", e)
          }
        if (ready > 0) {
          val socket = local.accept()
          clientSocket = socket
          Log.i(TAG, "$name: accepted over the abstract socket (adb)")
          return socket.inputStream to socket.outputStream
        }
      }

      if (tcp != null) {
        try {
          val socket = tcp.accept()
          // Control messages are small and latency-sensitive (a tray click should apply now, not
          // whenever Nagle decides enough bytes have accumulated).
          socket.tcpNoDelay = true
          tcpClient = socket
          Log.i(TAG, "$name: accepted over TCP from ${socket.inetAddress?.hostAddress}")
          return socket.getInputStream() to socket.getOutputStream()
        } catch (_: SocketTimeoutException) {
          // Normal: nothing connected within soTimeout, fall through and re-check cancellation.
        } catch (e: IOException) {
          if (!isCancelled()) throw e
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
    const val TAG = "ControlSocketServer"

    // Must match windows/host/control/ControlTransport.cpp's default port.
    const val DEFAULT_TCP_PORT = 27184
    const val TCP_BACKLOG = 4

    // See VideoSocketServer's identical constants for why this budget is 3s, not 500ms.
    const val MAX_BIND_ATTEMPTS = 30
    const val BIND_RETRY_DELAY_MS = 100L
    const val POLL_TIMEOUT_MS = 200
  }
}
