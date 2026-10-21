package io.github.abdoeid.phonecam.transport

import android.util.Log
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.SocketTimeoutException
import java.nio.charset.StandardCharsets
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Answers the PC's discovery probe: "is there a PhoneCam on this network, and where?"
 *
 * The shape is deliberately request/response rather than a periodic beacon. The PC
 * asks only when it needs an address (starting a wireless session), so a phone
 * that isn't being used costs nothing on the network -- and, more importantly,
 * the phone doesn't have to be broadcasting its address around continuously.
 *
 * The reply carries the ports the PC needs (control and video), so the PC
 * doesn't have to hardcode them, and the protocol version, so a mismatched pair
 * says so instead of misbehaving later.
 *
 * This runs for as long as the phone app is open (it belongs to CaptureService),
 * which is the same lifetime the control channel has.
 */
class DiscoveryResponder(
  private val controlPort: Int,
  private val videoPort: Int,
) {
  private val socket = DatagramSocket(null)
  private val running = AtomicBoolean(false)
  private var thread: Thread? = null

  /**
   * Binds the discovery port and serves probes until [stop] is called.
   *
   * Binding is best-effort: if the port is taken (another instance of the app,
   * say), discovery simply doesn't work -- the phone is still reachable by its
   * address, which is what the user can always fall back to.
   */
  fun start() {
    if (running.get()) return
    try {
      socket.reuseAddress = true
      socket.broadcast = true
      socket.bind(InetSocketAddress(DISCOVERY_PORT))
    } catch (e: IOException) {
      Log.w(TAG, "discovery could not bind port $DISCOVERY_PORT", e)
      return
    }
    running.set(true)
    thread = Thread({ serve() }, "DiscoveryResponder").apply { start() }
  }

  fun stop() {
    if (!running.compareAndSet(false, true)) return
    socket.close() // unblocks the receive
    thread?.join(500)
    thread = null
  }

  private fun serve() {
    val buffer = ByteArray(256)
    while (running.get()) {
      val packet = DatagramPacket(buffer, buffer.size)
      try {
        socket.soTimeout = RECEIVE_TIMEOUT_MS
        socket.receive(packet)
      } catch (_: SocketTimeoutException) {
        continue // normal: nobody asked
      } catch (_: IOException) {
        break // socket closed (stop())
      }
      if (!running.get()) break

      val request = String(packet.data, 0, packet.length, StandardCharsets.UTF_8)
      if (!request.trim().startsWith(PROBE_MAGIC)) continue

      val reply = "$REPLY_MAGIC $PROTOCOL_VERSION $controlPort $videoPort\n"
      val replyBytes = reply.toByteArray(StandardCharsets.UTF_8)
      val out = DatagramPacket(replyBytes, replyBytes.size, packet.address, packet.port)
      try {
        socket.send(out)
        Log.i(TAG, "answered a discovery probe from ${packet.address.hostAddress}")
      } catch (e: IOException) {
        Log.w(TAG, "failed to answer a discovery probe", e)
      }
    }
  }

  companion object {
    private const val TAG = "DiscoveryResponder"

    // Must match windows/host/transport/Discovery.cpp's constants.
    const val DISCOVERY_PORT = 27186
    const val PROBE_MAGIC = "PHONECAM_DISCOVER"
    const val REPLY_MAGIC = "PHONECAM_HERE"

    // The control protocol version this build speaks (proto/control.fbs).
    const val PROTOCOL_VERSION = 4

    private const val RECEIVE_TIMEOUT_MS = 1000
  }
}
