package io.github.abdoeid.phonecam.transport

import android.net.LocalServerSocket
import android.net.LocalSocket
import java.io.OutputStream

/**
 * Listens on the `phonecam_video` abstract socket -- bridged to the PC host via
 * `adb forward tcp:27183 localabstract:phonecam_video`, per docs/wire-protocol.md. [accept]
 * blocks until the PC connects; [close] is what unblocks a pending [accept] (LocalSocket
 * doesn't respond to thread interrupt the way NIO channels do -- closing the server socket is
 * what makes a blocked accept() throw and return control to the caller).
 */
class VideoSocketServer(name: String = "phonecam_video") {
  private val serverSocket = LocalServerSocket(name)
  private var clientSocket: LocalSocket? = null

  fun accept(): OutputStream {
    val socket = serverSocket.accept()
    clientSocket = socket
    return socket.outputStream
  }

  fun close() {
    try {
      clientSocket?.close()
    } catch (_: Exception) {
      // best-effort teardown
    }
    try {
      serverSocket.close()
    } catch (_: Exception) {
      // best-effort teardown
    }
    clientSocket = null
  }
}
