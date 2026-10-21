package io.github.abdoeid.phonecam.control

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionAuthTest {
  @Test
  fun matchesIndependentHmacVector() {
    assertEquals(
      "f6c60d821f74cb158ef33d27c90bbdb2f5daf93acc20a420f4215b8266346d0b",
      SessionAuth.computeHmac("ABCDEFGHJKLM", "phone-nonce-123", "pc-nonce-456"),
    )
  }

  @Test
  fun rejectsWrongCodeAndReplayedProof() {
    val proof = SessionAuth.computeHmac("ABCDEFGHJKLM", "phone-nonce-123", "pc-nonce-456")
    assertTrue(SessionAuth.matches(proof, proof))
    assertFalse(SessionAuth.matches(proof, ""))
    assertFalse(SessionAuth.matches(proof, "0".repeat(64)))
    assertFalse(SessionAuth.matches(proof, SessionAuth.computeHmac("WRONG", "phone-nonce-123", "pc-nonce-456")))
    assertFalse(SessionAuth.matches(proof, SessionAuth.computeHmac("ABCDEFGHJKLM", "new-challenge", "pc-nonce-456")))
  }
}
