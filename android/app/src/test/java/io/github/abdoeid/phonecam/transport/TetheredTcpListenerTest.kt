package io.github.abdoeid.phonecam.transport

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TetheredTcpListenerTest {
  @Test fun acceptsOnlyUsbTetherInterfacePrefixes() {
    for (name in listOf("rndis0", "usb0", "ncm0", "RNDIS1")) {
      assertTrue("$name should be treated as USB tethering", TetheredTcpListener.isUsbTetherInterfaceName(name))
    }
    for (name in listOf("wlan0", "rmnet_data0", "eth0", "lo")) {
      assertFalse("$name must not expose tethered listeners", TetheredTcpListener.isUsbTetherInterfaceName(name))
    }
  }
}
