package io.github.abdoeid.phonecam.control

import android.content.Context
import android.os.Build
import java.security.SecureRandom

/**
 * The PCs this phone trusts, and the QR pairing handshake with them.
 *
 * QR tokens are transient: only the derived per-PC key is persisted, in private
 * app preferences that are excluded from cloud backup and device transfer. The
 * handshake itself lives in [PairingSession] (pure, unit-tested); this class owns
 * storage and the transport-facing [reply].
 *
 * The pairing state is process-wide (one phone, one pending scan at a time), so
 * it lives in the companion object: the service, the discovery responder and the
 * UI all share it. [init] must run once, before any pairing happens.
 */
class TrustedPcs(context: Context) {
  private val prefs = context.getSharedPreferences("phonecam_trusted_pcs", Context.MODE_PRIVATE)

  data class Pc(val id: String, val name: String)

  fun list(): List<Pc> =
    prefs.getStringSet("ids", emptySet()).orEmpty().sorted().map { Pc(it, prefs.getString("name_$it", "PC") ?: "PC") }

  fun key(id: String): String =
    if (PairingSession.isHex(id, 32)) prefs.getString("key_$id", "") ?: "" else ""

  fun forget(id: String) = synchronized(lock) {
    session.cancel()
    message = "PC forgotten. It must scan a new QR before reconnecting."
    prefs.edit().putStringSet("ids", prefs.getStringSet("ids", emptySet()).orEmpty() - id)
      .remove("key_$id").remove("name_$id").commit()
  }

  fun scan(payload: String) {
    val fields = parseQr(payload)
    synchronized(lock) {
      session.scan(fields[0], fields[1], fields[2], System.nanoTime(), randomHex(16))
      message = "Waiting for the PC to confirm pairing..."
    }
  }

  /** Called by DiscoveryResponder, which replies only to the requesting socket. */
  fun reply(request: String, control: Int, video: Int): String? = synchronized(lock) {
    val f = request.trim().split(' ')
    if (f.size == 3 && f[0] == "PHONECAM_PAIR" && PairingSession.isHex(f[1], 32)) {
      return session.offer(f[2], control, video, Build.MODEL ?: "Phone", System.nanoTime())
    }
    if (f.size == 6 && f[0] == "PHONECAM_CONFIRM") {
      val current = session.pending ?: return null
      if (current.pcId != f[1] || phoneId != f[2] || current.nonce != f[3]) return null
      if (!PairingSession.isHex(f[4], 32)) return null
      if (!session.confirm(f[1], f[3], f[4], f[5], System.nanoTime())) return null
      if (!prefs.edit().putStringSet("ids", prefs.getStringSet("ids", emptySet()).orEmpty() + current.pcId)
        .putString("key_${current.pcId}", session.pairKey(current.token, current.pcId, current.nonce))
        .putString("name_${current.pcId}", current.name).commit()) {
        session.cancel()
        return null
      }
      message = "Paired with ${current.name}. This PC is remembered."
      val proof = session.pairedProof(f[4]) ?: return null
      return "PHONECAM_PAIRED $phoneId $proof\n"
    }
    if (f.size == 3 && f[0] == "PHONECAM_KNOWN" && PairingSession.isHex(f[1], 32)) {
      val stored = key(f[1])
      if (stored.isEmpty()) return null
      return session.known(stored, f[1], f[2], control, video)
    }
    null
  }

  companion object {
    private val lock = Any()

    @Volatile private var message: String = ""
      private set

    /** The phone's own identity, stable for the life of the installation. */
    lateinit var phoneId: String
      private set

    @Volatile private var session: PairingSession = PairingSession("")

    /** Loads the phone identity and prepares the handshake. Call once, at startup. */
    fun init(context: Context) = synchronized(lock) {
      val prefs = context.getSharedPreferences("phonecam_trusted_pcs", Context.MODE_PRIVATE)
      val id = prefs.getString("phone_id", null) ?: randomHex(16).also {
        prefs.edit().putString("phone_id", it).commit()
      }
      phoneId = id
      session = PairingSession(id)
    }

    /** The QR payload format: version, PC id, one-time token, hex-encoded PC name. */
    fun parseQr(payload: String): List<String> {
      require(payload.length <= 1024) { "Not a PhoneCam PC QR code" }
      val fields = payload.split(':')
      require(fields.size == 5 && fields[0] == "phonecam-pair" && fields[1] == "1" &&
        PairingSession.isHex(fields[2], 32) && PairingSession.isHex(fields[3], 64) &&
        fields[4].length in 2..256 && fields[4].length % 2 == 0 &&
        fields[4].all { it in "0123456789abcdef" }) { "Not a PhoneCam PC QR code" }
      val name = fields[4].chunked(2).map { it.toInt(16).toByte() }.toByteArray().toString(Charsets.UTF_8)
      require(name.none { it.isISOControl() }) { "Invalid PC name" }
      return listOf(fields[2], fields[3], name)
    }

    fun status(): String = synchronized(lock) { message }

    fun cancelPending() = synchronized(lock) { message = "" }

    fun isPairingPending(): Boolean = synchronized(lock) {
      val current = session.pending ?: return false
      if (current.confirmed) return false
      if (System.nanoTime() >= current.expiresAtNanos) {
        session.cancel()
        message = "QR pairing expired. Scan a new QR from the PC."
        return false
      }
      true
    }

    private fun randomHex(bytes: Int): String =
      ByteArray(bytes).also { SecureRandom().nextBytes(it) }
        .joinToString("") { "%02x".format(it.toInt() and 255) }
  }
}
