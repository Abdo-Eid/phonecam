package io.github.abdoeid.phonecam.control

/**
 * The QR pairing handshake, with no Android dependencies.
 *
 * The laptop shows a QR containing a PC identity and a one-time token. After the
 * phone scans it, both sides derive a per-PC/per-phone key and prove it to each
 * other with HMACs over fresh challenges. Every message is bound to the PC, the
 * phone, the phone's nonce and the challenged value, so a recorded exchange
 * cannot be replayed against a different session.
 *
 * Storage (the phone's preferences) and the transport (UDP discovery) live
 * outside this class, which keeps the security-critical arithmetic testable on
 * the JVM. [TrustedPcs] owns the storage and delegates the handshake here.
 */
class PairingSession(val phoneId: String) {
  data class Pending(
    val pcId: String,
    val token: String,
    val name: String,
    val nonce: String,
    val expiresAtNanos: Long,
    val confirmed: Boolean = false,
  )

  @Volatile var pending: Pending? = null
    private set

  /** Records a scanned QR. Returns false if the payload is not a usable pairing code. */
  fun scan(pcId: String, token: String, name: String, nowNanos: Long, nonce: String): Boolean {
    if (!isHex(pcId, 32) || !isHex(token, 64)) return false
    pending = Pending(pcId, token, name, nonce, nowNanos + ENROLLMENT_WINDOW_NANOS)
    return true
  }

  fun cancel() {
    pending = null
  }

  fun confirmed(): Boolean = pending?.confirmed == true

  fun expired(nowNanos: Long): Boolean {
    val current = pending ?: return false
    if (nowNanos < current.expiresAtNanos) return false
    pending = null
    return true
  }

  /** The key both sides derive from the QR's one-time token. */
  fun pairKey(token: String, pcId: String, phoneNonce: String): String =
    SessionAuth.computeMessageHmac(token, "pair-key|$pcId|$phoneId|$phoneNonce")

  /**
   * Answers the PC's pairing challenge with the phone's offer. Returns null when
   * no matching scan is pending or the enrollment window has closed.
   */
  fun offer(challenge: String, controlPort: Int, videoPort: Int, modelName: String, nowNanos: Long): String? {
    val current = pending ?: return null
    if (current.pcId.isEmpty() || current.confirmed) return null
    if (challenge.isEmpty()) return null
    if (nowNanos >= current.expiresAtNanos) {
      pending = null
      return null
    }
    val name = encode(modelName.take(32))
    val key = pairKey(current.token, current.pcId, current.nonce)
    val proof =
      SessionAuth.computeMessageHmac(
        key,
        "offer|${current.pcId}|$phoneId|${current.nonce}|$challenge|$controlPort|$videoPort|$name",
      )
    return "PHONECAM_OFFER $phoneId ${current.nonce} $controlPort $videoPort $name $proof\n"
  }

  /**
   * Confirms the PC's identity: the PC has to prove it knows the same key. On
   * success the pairing is marked confirmed, so the enrollment window closes.
   */
  fun confirm(pcId: String, phoneNonce: String, challenge: String, proof: String, nowNanos: Long): Boolean {
    val current = pending ?: return false
    if (current.pcId != pcId || current.nonce != phoneNonce) return false
    if (nowNanos >= current.expiresAtNanos) return false
    val key = pairKey(current.token, current.pcId, current.nonce)
    val expected = SessionAuth.computeMessageHmac(key, "confirm|${current.pcId}|$phoneId|${current.nonce}|$challenge")
    if (!SessionAuth.matches(expected, proof)) return false
    pending = current.copy(confirmed = true)
    return true
  }

  /** The PC's final acknowledgement of a completed pairing. */
  fun pairedProof(challenge: String): String? {
    val current = pending ?: return null
    if (!current.confirmed) return null
    val key = pairKey(current.token, current.pcId, current.nonce)
    return SessionAuth.computeMessageHmac(key, "paired|${current.pcId}|$phoneId|${current.nonce}|$challenge")
  }

  /**
   * Answers the PC's rediscovery probe: it must prove it knows the key that was
   * stored for it, bound to the fresh challenge and the ports it is offering.
   */
  fun known(key: String, pcId: String, challenge: String, controlPort: Int, videoPort: Int): String? {
    if (key.isEmpty() || !isHex(pcId, 32)) return null
    val proof =
      SessionAuth.computeMessageHmac(
        key,
        "known|$pcId|$phoneId|$challenge|$controlPort|$videoPort",
      )
    return "PHONECAM_KNOWN_HERE $phoneId $controlPort $videoPort $proof\n"
  }

  companion object {
    // Two minutes: long enough to fetch the phone and open the app, short enough
    // that a screenshot of the QR is not a long-lived credential.
    const val ENROLLMENT_WINDOW_NANOS = 120_000_000_000L

    fun isHex(text: String, length: Int): Boolean =
      text.length == length && text.all { it in "0123456789abcdef" }

    fun encode(text: String): String =
      text.toByteArray(Charsets.UTF_8).joinToString("") { "%02x".format(it.toInt() and 255) }
  }
}
