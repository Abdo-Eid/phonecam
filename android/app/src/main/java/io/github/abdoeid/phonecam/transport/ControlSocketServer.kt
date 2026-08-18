package io.github.abdoeid.phonecam.transport

import android.net.LocalServerSocket
import android.net.LocalSocket
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * Listens on the `phonecam_control` abstract socket -- bridged to the PC host via
 * `adb forward tcp:27184 localabstract:phonecam_control`, per docs/wire-protocol.md. Bidirectional
 * (unlike [VideoSocketServer]): the PC sends commands, the phone sends capabilities/telemetry back
 * on the same connection. Same open()-with-retry and close()-unblocks-accept() shape as
 * [VideoSocketServer] -- see its docs for why (Phase 3B bind-race fix).
 */
class ControlSocketServer(private val name: String = "phonecam_control") {
  private var serverSocket: LocalServerSocket? = null
  private var clientSocket: LocalSocket? = null

  fun open() {
    var lastError: IOException? = null
    repeat(MAX_BIND_ATTEMPTS) {
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

  fun accept(): Pair<InputStream, OutputStream> {
    val socket = serverSocket!!.accept()
    clientSocket = socket
    return socket.inputStream to socket.outputStream
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
    const val MAX_BIND_ATTEMPTS = 10
    const val BIND_RETRY_DELAY_MS = 50L
  }
}
