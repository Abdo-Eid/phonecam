package io.github.abdoeid.phonecam.control

import java.io.EOFException
import java.io.IOException
import java.net.Socket
import java.security.SecureRandom

/** Connection-time authorization only: the subsequent H.264 stream stays plaintext. */
class NetworkVideoAuth {
  private data class Session(val peer: String, val code: String, val phoneNonce: String, val pcNonce: String)

  @Volatile private var session: Session? = null

  fun startSession(peer: String, code: String, phoneNonce: String, pcNonce: String) {
    require(code.isNotEmpty() && phoneNonce.isNotEmpty() && pcNonce.isNotEmpty())
    session = Session(peer, code, phoneNonce, pcNonce)
  }

  fun clear() {
    session = null
  }

  /** A rejected socket never reaches the encoder; each attempt has a bounded deadline. */
  fun authenticate(socket: Socket, isCancelled: () -> Boolean = { false }): Boolean {
    val current = session ?: return false
    if (socket.inetAddress.hostAddress != current.peer || isCancelled()) return false
    val deadline = System.nanoTime() + TIMEOUT_MS * 1_000_000L
    return try {
      val nonce = ByteArray(16).also { SecureRandom().nextBytes(it) }
      val out = socket.getOutputStream()
      out.write(MAGIC)
      out.write(nonce)
      out.flush()
      val proof = ByteArray(64)
      var offset = 0
      while (offset < proof.size) {
        val remainingMs = (deadline - System.nanoTime()) / 1_000_000L
        if (remainingMs <= 0 || isCancelled() || session !== current) return false
        socket.soTimeout = remainingMs.toInt().coerceAtLeast(1)
        val count = socket.getInputStream().read(proof, offset, proof.size - offset)
        if (count < 0) throw EOFException("video authentication closed")
        offset += count
      }
      val expected = computeProof(current.code, current.phoneNonce, current.pcNonce, hex(nonce))
      if (!SessionAuth.matches(expected, String(proof, Charsets.US_ASCII)) ||
        session !== current || isCancelled() || System.nanoTime() >= deadline) return false
      socket.soTimeout = 0
      out.write(1) // positive acknowledgement, before the ordinary video framing
      out.flush()
      true
    } catch (_: IOException) {
      false
    }
  }

  companion object {
    private val MAGIC = byteArrayOf(0x50, 0x43, 0x56, 0x31) // PCV1
    private const val TIMEOUT_MS = 2_000

    fun computeProof(code: String, phoneNonce: String, pcNonce: String, videoNonce: String): String =
      SessionAuth.computeMessageHmac(code, "phonecam-video-v1|$phoneNonce|$pcNonce|$videoNonce")

    private fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }
  }
}
