package io.github.abdoeid.phonecam.transport

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * Listens on the `phonecam_control` abstract socket -- bridged to the PC host via
 * `adb forward tcp:27184 localabstract:phonecam_control`, per docs/wire-protocol.md. Bidirectional
 * (unlike [VideoSocketServer]): the PC sends commands, the phone sends capabilities/telemetry back
 * on the same connection. Same open()-with-retry and poll-based-cancellable-accept() shape as
 * [VideoSocketServer] -- see its class doc comment for why accept() polls with a timeout instead
 * of depending entirely on close() to interrupt a blocked accept().
 */
class ControlSocketServer(private val name: String = "phonecam_control") {
  private var serverSocket: LocalServerSocket? = null
  private var clientSocket: LocalSocket? = null

  // isCancelled: see VideoSocketServer.open()'s doc comment -- same reasoning, same fix.
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

  // isCancelled: see VideoSocketServer.accept()'s doc comment -- same reasoning, same fix.
  fun accept(isCancelled: () -> Boolean = { false }): Pair<InputStream, OutputStream> {
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
          throw IOException("poll failed, socket likely closed", e)
        }
      if (ready > 0) {
        val socket = server.accept()
        clientSocket = socket
        return socket.inputStream to socket.outputStream
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
    // See VideoSocketServer's identical constants for why this budget is 3s, not 500ms.
    const val MAX_BIND_ATTEMPTS = 30
    const val BIND_RETRY_DELAY_MS = 100L
    const val POLL_TIMEOUT_MS = 200
  }
}
