package io.github.abdoeid.phonecam.control

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class TrustedPcsTest {
  private val pc = "b".repeat(32)
  private val token = "a".repeat(64)
  private val qr = "phonecam-pair:1:$pc:$token:4d79205043" // My PC
  private val phoneId = "c".repeat(32)
  private val nonce = "d".repeat(32)
  private val challenge = "e".repeat(32)

  @Test fun readsPhoneCamQrOnly() {
    assertEquals(listOf(pc, token, "My PC"), TrustedPcs.parseQr(qr))
    for (bad in listOf("https://example.com", qr.replace(":1:", ":2:"), "$qr:extra",
      qr.replace(token, "short"), qr.replace(pc, "z".repeat(32)),
      qr.substringBeforeLast(':') + ":00", "x".repeat(1025))) {
      assertThrows(IllegalArgumentException::class.java) { TrustedPcs.parseQr(bad) }
    }
  }

  @Test fun pairKeyMatchesIndependentHmacAndBindsPhoneAndNonce() {
    val session = PairingSession(phoneId)
    val key = session.pairKey(token, pc, nonce)
    assertEquals("cd1e4d25608686867ed87b7f599fb28d1fb9715867c6b7986315f2bc4df32e11", key)
    assertNotEquals(key, session.pairKey(token, pc, "e".repeat(32)))
    assertNotEquals(key, session.pairKey(token, pc, "f".repeat(32)))
  }

  @Test fun enrollmentRequiresAScanAndExpires() {
    val session = PairingSession(phoneId)
    val now = System.nanoTime()
    assertNull(session.offer(challenge, 27184, 27183, "Model", now))
    assertFalse(session.scan(pc, "short-token", "My PC", now, nonce))
    assertTrue(session.scan(pc, token, "My PC", now, nonce))
    assertTrue(session.offer(challenge, 27184, 27183, "Model", now)!!.startsWith("PHONECAM_OFFER $phoneId $nonce"))
    // Past the enrollment window the PC can no longer enroll.
    val late = now + PairingSession.ENROLLMENT_WINDOW_NANOS + 1
    assertNull(session.offer(challenge, 27184, 27183, "Model", late))
    assertFalse(session.confirm(pc, nonce, challenge, "0".repeat(64), late))
  }

  @Test fun confirmationRequiresTheMatchingProof() {
    val session = PairingSession(phoneId)
    session.scan(pc, token, "My PC", System.nanoTime(), nonce)
    val key = session.pairKey(token, pc, nonce)
    val good = SessionAuth.computeMessageHmac(key, "confirm|$pc|$phoneId|$nonce|$challenge")
    assertFalse(session.confirm(pc, nonce, challenge, "0".repeat(64), System.nanoTime()))
    assertFalse(session.confirm("f".repeat(32), nonce, challenge, good, System.nanoTime()))
    assertTrue(session.confirm(pc, nonce, challenge, good, System.nanoTime()))
    assertTrue(session.confirmed())
    // The paired proof is bound to the same key and challenge.
    val paired = session.pairedProof(challenge)!!
    assertTrue(SessionAuth.matches(
      SessionAuth.computeMessageHmac(key, "paired|$pc|$phoneId|$nonce|$challenge"), paired))
  }

  @Test fun offerIsBoundToTheChallengeAndPorts() {
    val session = PairingSession(phoneId)
    val now = System.nanoTime()
    session.scan(pc, token, "My PC", now, nonce)
    val first = session.offer(challenge, 27184, 27183, "Model", now)!!
    val second = session.offer("f".repeat(32), 27184, 27183, "Model", now)!!
    assertNotEquals(first, second)
    val key = session.pairKey(token, pc, nonce)
    val name = PairingSession.encode("Model")
    assertTrue(SessionAuth.matches(
      SessionAuth.computeMessageHmac(key, "offer|$pc|$phoneId|$nonce|$challenge|27184|27183|$name"),
      first.split(' ').last().trim()))
  }

  @Test fun rememberedDiscoveryIsBoundToTheStoredKey() {
    val session = PairingSession(phoneId)
    val key = "1".repeat(64)
    val reply = session.known(key, pc, challenge, 27184, 27183)!!
    val proof = reply.split(' ').last().trim()
    assertTrue(SessionAuth.matches(
      SessionAuth.computeMessageHmac(key, "known|$pc|$phoneId|$challenge|27184|27183"), proof))
    // A different key must not verify.
    assertFalse(SessionAuth.matches(
      SessionAuth.computeMessageHmac("2".repeat(64), "known|$pc|$phoneId|$challenge|27184|27183"), proof))
  }
}
