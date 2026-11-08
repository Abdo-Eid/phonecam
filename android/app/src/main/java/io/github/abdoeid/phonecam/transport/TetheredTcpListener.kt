package io.github.abdoeid.phonecam.transport

import android.util.Log
import java.io.IOException
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.net.SocketTimeoutException
import java.util.Collections
import java.util.Locale

/**
 * A TCP listener bound only to an IPv4 address on Android's USB-tethering interface.
 * It deliberately never binds wildcard/all-interfaces: Wi-Fi and cellular addresses
 * must not expose the camera control or video ports.
 *
 * Android and OEMs use names such as rndis0, usb0, and ncm0 for USB tethering. The
 * listener refreshes its bind address before each accept, so tethering may be enabled
 * after the app starts and address changes are picked up without restarting the app.
 */
internal class TetheredTcpListener(
  private val port: Int,
  private val backlog: Int,
  private val tag: String,
) {
  @Volatile private var server: ServerSocket? = null
  @Volatile private var boundAddress: InetAddress? = null
  val isBound: Boolean get() = server?.isClosed == false

  fun open() {
    refresh()
  }

  fun accept(timeoutMs: Int): Socket? {
    refresh()
    val current = server
    if (current == null) {
      try {
        Thread.sleep(timeoutMs.toLong())
      } catch (_: InterruptedException) {
        Thread.currentThread().interrupt()
      }
      return null
    }
    return try {
      current.soTimeout = timeoutMs
      current.accept()
    } catch (_: SocketTimeoutException) {
      null
    } catch (e: IOException) {
      if (server === current) {
        Log.w(tag, "USB-tethered listener on port $port stopped; it will rebind if tethering returns", e)
        closeServer()
      }
      null
    }
  }

  @Synchronized
  private fun refresh() {
    val address = findUsbTetherAddress()
    if (address == boundAddress && server?.isClosed == false) return

    closeServer()
    if (address == null) return
    try {
      server = ServerSocket().apply {
        reuseAddress = true
        bind(InetSocketAddress(address, port), backlog)
        soTimeout = ACCEPT_POLL_MS
      }
      boundAddress = address
      Log.i(tag, "USB-tethered listener bound to ${address.hostAddress}:$port")
    } catch (e: IOException) {
      Log.w(tag, "USB-tethered listener could not bind ${address.hostAddress}:$port; will retry", e)
      closeServer()
    }
  }

  @Synchronized
  fun close() = closeServer()

  @Synchronized
  private fun closeServer() {
    try {
      server?.close()
    } catch (_: IOException) {
      // Best-effort teardown.
    }
    server = null
    boundAddress = null
  }

  private fun findUsbTetherAddress(): InetAddress? {
    return try {
      val interfaces = NetworkInterface.getNetworkInterfaces() ?: return null
      Collections.list(interfaces)
        .asSequence()
        .filter { it.isUp && isUsbTetherInterfaceName(it.name) }
        .sortedBy { it.name }
        .flatMap { networkInterface ->
          Collections.list(networkInterface.inetAddresses).asSequence()
        }
        .filterIsInstance<Inet4Address>()
        .firstOrNull { !it.isLoopbackAddress && !it.isLinkLocalAddress }
    } catch (e: SocketException) {
      Log.w(tag, "could not enumerate USB-tethering interfaces", e)
      null
    }
  }

  companion object {
    private const val ACCEPT_POLL_MS = 200
    private val USB_TETHER_PREFIXES = listOf("rndis", "usb", "ncm")

    internal fun isUsbTetherInterfaceName(name: String): Boolean {
      val normalized = name.lowercase(Locale.ROOT)
      return USB_TETHER_PREFIXES.any(normalized::startsWith)
    }
  }
}
