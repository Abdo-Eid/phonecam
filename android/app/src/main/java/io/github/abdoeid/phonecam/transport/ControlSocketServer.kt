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
import java.net.Socket

/**
 * Listens for the PC's control connection on the `phonecam_control` abstract socket (bridged via
 * `adb forward tcp:27184 localabstract:phonecam_control`) and on a plain TCP listener at
 * [tcpPort], returning whichever connects first -- same dual-listener shape as
 * [VideoSocketServer], see its class doc for the full reasoning.
 *
 * Bidirectional (unlike [VideoSocketServer]): the PC sends commands, the phone sends
 * capabilities/telemetry back on the same connection.
 *
 * The TCP listener carries camera controls over USB tethering. Both control and video use TCP
 * to the same tethered phone.
 */
class ControlSocketServer(
  private val name: String = "phonecam_control",
  private val tcpPort: Int = DEFAULT_TCP_PORT,
) {
  private var serverSocket: LocalServerSocket? = null
  private var clientSocket: LocalSocket? = null
  private val tcpListener = TetheredTcpListener(tcpPort, TCP_BACKLOG, TAG)
  private var tcpClient: Socket? = null

  /** The PC address for tethered TCP, or null for an ADB-forwarded abstract socket. */
  var lastAcceptedAddress: String? = null
    private set

  // The USB-tethered listener binds only to the detected tethering interface. The abstract
  // socket remains available independently for USB debugging (ADB).
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

    if (tcpListenerIsBound()) {
      Log.w(TAG, "$name: abstract socket bind failed, serving over USB-tethered TCP only", lastError)
      return
    }
    throw lastError ?: IOException("failed to bind $name")
  }

  // isCancelled / alternating-poll rationale: see VideoSocketServer.accept()'s doc comment.
  fun accept(isCancelled: () -> Boolean = { false }): Pair<InputStream, OutputStream> {
    val local = serverSocket
    if (local == null && !tcpListenerIsBound()) throw IOException("$name: accept() with no bound listener")

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
          // The abstract socket's peer is the phone's own adb daemon -- there
          // is no meaningful address to stream to.
          lastAcceptedAddress = null
          Log.i(TAG, "$name: accepted over the abstract socket (adb)")
          return socket.inputStream to socket.outputStream
        }
      }

      if (!isCancelled()) {
        try {
          val socket = tcpListener.accept(POLL_TIMEOUT_MS) ?: continue
          // Control messages are small and latency-sensitive (a tray click should apply now, not
          // whenever Nagle decides enough bytes have accumulated).
          socket.tcpNoDelay = true
          tcpClient = socket
          lastAcceptedAddress = socket.inetAddress?.hostAddress
          Log.i(TAG, "$name: accepted over TCP from ${socket.inetAddress?.hostAddress}")
          return socket.getInputStream() to socket.getOutputStream()
        } catch (e: IOException) {
          if (!isCancelled()) throw e
        }
      }
    }
    throw IOException("accept() cancelled")
  }

  /** Ends just the accepted session; the listeners stay bound for the next PC. */
  fun closeClient() {
    try {
      clientSocket?.close()
    } catch (_: Exception) {
      // best-effort teardown
    }
    try {
      tcpClient?.close()
    } catch (_: Exception) {
      // best-effort teardown
    }
    clientSocket = null
    tcpClient = null
    lastAcceptedAddress = null
  }

  fun close() {
    closeClient()
    try {
      serverSocket?.close()
    } catch (_: Exception) {
      // best-effort teardown
    }
    serverSocket = null
    tcpListener.close()
  }

  private fun tcpListenerIsBound() = tcpListener.isBound

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
