package io.github.abdoeid.phonecam.transport

import android.net.LocalServerSocket
import android.net.LocalSocket
import java.io.IOException
import java.io.OutputStream

/**
 * Listens on the `phonecam_video` abstract socket -- bridged to the PC host via
 * `adb forward tcp:27183 localabstract:phonecam_video`, per docs/wire-protocol.md. [accept]
 * blocks until the PC connects; [close] is what unblocks a pending [accept] (LocalSocket
 * doesn't respond to thread interrupt the way NIO channels do -- closing the server socket is
 * what makes a blocked accept() throw and return control to the caller).
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
   */
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

  fun accept(): OutputStream {
    val socket = serverSocket!!.accept()
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
