package io.github.abdoeid.phonecam

import android.content.Intent
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import io.github.abdoeid.phonecam.theme.PhoneCamTheme
import io.github.abdoeid.phonecam.transport.AoaAccessoryTransport

private const val TAG = "PhoneCamAoa"

class MainActivity : ComponentActivity() {
  // Phase 7 checkpoint only (see AoaAccessoryTransport's doc comment) -- not yet used by the real
  // capture pipeline, which still runs over the Phase 3 adb transport.
  private val aoaTransport = AoaAccessoryTransport()

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    enableEdgeToEdge()
    setContent {
      PhoneCamTheme { Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) { MainNavigation() } }
    }

    handleAccessoryIntent(intent)
  }

  // singleTop (see AndroidManifest.xml) routes a second USB_ACCESSORY_ATTACHED here instead of
  // spawning a new Activity instance while the app's already running.
  override fun onNewIntent(intent: Intent) {
    super.onNewIntent(intent)
    handleAccessoryIntent(intent)
  }

  private fun handleAccessoryIntent(intent: Intent) {
    val usbManager = getSystemService(UsbManager::class.java)

    // The intent extra is only present for the actual ATTACHED broadcast (fired once per physical
    // attach). A cold/plain launch while an accessory is already attached -- e.g. the app was
    // force-stopped after the accessory connected, or this activity starts some other way while
    // one's live -- carries no such extra, so fall back to querying what's currently attached.
    // This is Android's own recommended pattern for accessory handling, not just a testing
    // convenience, though it does also mean re-testing doesn't require a fresh physical replug.
    val accessory =
      if (intent.action == UsbManager.ACTION_USB_ACCESSORY_ATTACHED) {
        if (Build.VERSION.SDK_INT >= 33) {
          intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java)
        } else {
          @Suppress("DEPRECATION") intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY)
        }
      } else {
        usbManager.accessoryList?.firstOrNull()
      }
    if (accessory == null) return
    if (aoaTransport.isOpen) return  // already holding this (or another) accessory open

    val ok = aoaTransport.openAndSendCheckpoint(usbManager, accessory)
    Log.i(TAG, "AOA checkpoint heartbeat: ${if (ok) "started" else "FAILED"} (accessory=${accessory.model})")
  }

  override fun onDestroy() {
    super.onDestroy()
    aoaTransport.close()
  }
}
