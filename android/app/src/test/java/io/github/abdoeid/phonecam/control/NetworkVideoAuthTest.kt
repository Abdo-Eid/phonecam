package io.github.abdoeid.phonecam.control

import java.io.DataInputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NetworkVideoAuthTest {
  private val code = "ABCDEFGHJKLM"
  private val phone = "phone-nonce-123"
  private val pc = "pc-nonce-456"

  private fun ready() = NetworkVideoAuth().apply { startSession("127.0.0.1", code, phone, pc) }

  private fun attempt(auth: NetworkVideoAuth, client: (Socket) -> Unit): Boolean {
    val executor = Executors.newSingleThreadExecutor()
    try {
      ServerSocket(0, 1, InetAddress.getByName("127.0.0.1")).use { server ->
        val accepted = executor.submit<Boolean> {
          server.accept().use { socket ->
            val ok = auth.authenticate(socket)
            if (ok) socket.getOutputStream().write(0x9c) // simulated first video byte
            ok
          }
        }
        Socket("127.0.0.1", server.localPort).use { socket ->
          socket.soTimeout = 4000
          client(socket)
        }
        return accepted.get(5, TimeUnit.SECONDS)
      }
    } finally {
      executor.shutdownNow()
    }
  }

  private fun challenge(socket: Socket): String {
    val header = ByteArray(20)
    DataInputStream(socket.getInputStream()).readFully(header)
    assertEquals("PCV1", String(header.copyOfRange(0, 4), Charsets.US_ASCII))
    return header.copyOfRange(4, 20).joinToString("") { "%02x".format(it.toInt() and 0xff) }
  }

  private fun send(socket: Socket, proof: String) {
    socket.getOutputStream().write(proof.toByteArray(Charsets.US_ASCII))
    socket.getOutputStream().flush()
  }

  @Test fun matchesIndependentVideoHmacVector() {
    assertEquals(
      "be207603bc8e58f89611cf9ff820e3a2d19ca7bb3ff5257550c85a65fe38b95d",
      NetworkVideoAuth.computeProof(code, phone, pc, "00112233445566778899aabbccddeeff"),
    )
  }

  @Test fun rejectsWithoutControlOrFromDifferentPeer() {
    assertFalse(attempt(NetworkVideoAuth()) { assertEquals(-1, it.getInputStream().read()) })
    val auth = NetworkVideoAuth().apply { startSession("192.0.2.1", code, phone, pc) }
    assertFalse(attempt(auth) { assertEquals(-1, it.getInputStream().read()) })
  }

  @Test fun wrongProofDoesNotLeakMediaOrPreventNextConnection() {
    val auth = ready()
    assertFalse(attempt(auth) { socket ->
      val nonce = challenge(socket)
      send(socket, NetworkVideoAuth.computeProof("WRONG", phone, pc, nonce))
      assertEquals(-1, socket.getInputStream().read())
    })
    assertTrue(attempt(auth) { socket ->
      send(socket, NetworkVideoAuth.computeProof(code, phone, pc, challenge(socket)))
      assertEquals(1, socket.getInputStream().read())
      assertEquals(0x9c, socket.getInputStream().read())
    })
  }

  @Test fun replayAndOldControlSessionAreRejected() {
    val auth = ready()
    var oldProof = ""
    assertTrue(attempt(auth) { socket ->
      oldProof = NetworkVideoAuth.computeProof(code, phone, pc, challenge(socket))
      send(socket, oldProof)
      assertEquals(1, socket.getInputStream().read())
    })
    assertFalse(attempt(auth) { socket ->
      challenge(socket)
      send(socket, oldProof)
      assertEquals(-1, socket.getInputStream().read())
    })
    auth.startSession("127.0.0.1", code, "new-phone-nonce", pc)
    assertFalse(attempt(auth) { socket ->
      send(socket, NetworkVideoAuth.computeProof(code, phone, pc, challenge(socket)))
      assertEquals(-1, socket.getInputStream().read())
    })
  }

  @Test fun controlDisconnectDuringHandshakeRevokesProof() {
    val auth = ready()
    assertFalse(attempt(auth) { socket ->
      val proof = NetworkVideoAuth.computeProof(code, phone, pc, challenge(socket))
      auth.clear()
      send(socket, proof)
      assertEquals(-1, socket.getInputStream().read())
    })
  }

  @Test fun silentAndPartialClientsTimeOutWithoutMedia() {
    for (partial in listOf(false, true)) {
      assertFalse(attempt(ready()) { socket ->
        challenge(socket)
        if (partial) send(socket, "123")
        assertEquals(-1, socket.getInputStream().read())
      })
    }
  }

  @Test fun controlLoginProofCannotAuthorizeVideo() {
    assertFalse(attempt(ready()) { socket ->
      challenge(socket)
      send(socket, SessionAuth.computeHmac(code, phone, pc))
      assertEquals(-1, socket.getInputStream().read())
    })
  }

  @Test fun fragmentedValidProofWorks() {
    assertTrue(attempt(ready()) { socket ->
      val proof = NetworkVideoAuth.computeProof(code, phone, pc, challenge(socket))
      for (chunk in proof.chunked(3)) send(socket, chunk)
      assertEquals(1, socket.getInputStream().read())
      assertEquals(0x9c, socket.getInputStream().read())
    })
  }
}
