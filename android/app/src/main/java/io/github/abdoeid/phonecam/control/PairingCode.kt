package io.github.abdoeid.phonecam.control

import android.content.Context
import android.util.Base64
import java.security.SecureRandom

/**
 * The phone's pairing code: the shared secret that lets a PC prove it is allowed to drive the
 * camera.
 *
 * Why a code at all: over Wi-Fi, anyone on the same network can reach the phone's control port.
 * Without this, a stranger could connect, take the video feed and move the camera (and the
 * torch). With it, a PC has to know the code before the phone will do anything.
 *
 * The code is never sent over the wire -- the PC proves it knows the code with an
 * HMAC over a fresh nonce (see [ControlChannel]'s handshake), so someone watching the traffic
 * can't harvest it, and a recorded handshake can't be replayed.
 *
 * It is generated on first run and stored in the app's preferences. The user sees it in the
 * app (that's how it gets onto the PC) and can replace it with their own.
 *
 * Control and TCP video connections are authenticated; subsequent commands and media are
 * not encrypted or individually authenticated. Use a trusted network: WPA2 is not end-to-end
 * protection against a compromised access point or an active peer.
 */
class PairingCode(context: Context) {
  private val preferences = context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)

  /** The current code, generating one on first use. */
  var code: String
    get() {
      val existing = preferences.getString(KEY_CODE, null)
      if (existing != null && existing.isNotEmpty()) return existing
      val generated = generate()
      preferences.edit().putString(KEY_CODE, generated).apply()
      return generated
    }
    set(value) {
      val trimmed = value.trim()
      preferences.edit().putString(KEY_CODE, trimmed).apply()
    }

  /** Replaces the code with a fresh random one. */
  fun regenerate(): String {
    val generated = generate()
    preferences.edit().putString(KEY_CODE, generated).apply()
    return generated
  }

  private fun generate(): String {
    // A readable alphabet (no ambiguous characters) and a length that is long enough to
    // matter but short enough to read out loud or type: 12 symbols out of a 32-character
    // alphabet is 60 bits.
    val alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
    val random = SecureRandom()
    val builder = StringBuilder(12)
    for (i in 0 until 12) {
      builder.append(alphabet[random.nextInt(alphabet.length)])
    }
    return builder.toString()
  }

  companion object {
    private const val PREFERENCES_NAME = "phonecam_pairing"
    private const val KEY_CODE = "pairing_code"
  }
}

/**
 * The proof of knowing the code: HMAC-SHA256(code, phoneNonce || pcNonce), hex-encoded.
 *
 * Both nonces are fresh per session, so a captured handshake proves nothing about a later
 * one. The phone's nonce comes first, so the PC cannot precompute a valid answer before it
 * has seen the challenge.
 */
object SessionAuth {
  fun computeHmac(code: String, phoneNonce: String, pcNonce: String): String {
    return computeMessageHmac(code, "$phoneNonce|$pcNonce")
  }

  fun computeMessageHmac(code: String, message: String): String {
    val mac = javax.crypto.Mac.getInstance("HmacSHA256")
    mac.init(javax.crypto.spec.SecretKeySpec(code.toByteArray(Charsets.UTF_8), "HmacSHA256"))
    return toHex(mac.doFinal(message.toByteArray(Charsets.UTF_8)))
  }

  /** Constant-time comparison: a timing difference would leak the expected value. */
  fun matches(expected: String, actual: String): Boolean {
    val a = expected.toByteArray(Charsets.UTF_8)
    val b = actual.toByteArray(Charsets.UTF_8)
    if (a.size != b.size) return false
    var diff = 0
    for (i in a.indices) {
      diff = diff or (a[i].toInt() xor b[i].toInt())
    }
    return diff == 0
  }

  private fun toHex(bytes: ByteArray): String {
    val out = StringBuilder(bytes.size * 2)
    for (b in bytes) {
      val value = b.toInt() and 0xFF
      out.append(HEX[value ushr 4])
      out.append(HEX[value and 0x0F])
    }
    return out.toString()
  }

  private const val HEX = "0123456789abcdef"

  /** A fresh nonce for one side of the handshake. */
  fun newNonce(): String {
    val bytes = ByteArray(16)
    SecureRandom().nextBytes(bytes)
    return Base64.encodeToString(bytes, Base64.NO_WRAP)
  }
}
