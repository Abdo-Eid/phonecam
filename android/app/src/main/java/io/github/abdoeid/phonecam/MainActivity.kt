package io.github.abdoeid.phonecam

import android.content.Intent
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import io.github.abdoeid.phonecam.capture.CaptureService
import io.github.abdoeid.phonecam.theme.PhoneCamTheme

class MainActivity : ComponentActivity() {
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

  // Auto-detect: an AOA accessory attach starts capture on its own, with no Start-button tap
  // needed -- this is what lets AOA work with USB debugging off (Phase 7's whole point). Forwards
  // to CaptureService (ACTION_START_AOA) rather than owning a transport/controller here, so an
  // AOA-triggered session gets the exact same foreground-service/wake-lock/error-handling
  // treatment the normal Start-button path already has via ACTION_START -- see CaptureService's
  // class doc. CaptureService itself ignores a redundant dispatch while already streaming.
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

    val serviceIntent =
      Intent(this, CaptureService::class.java).apply {
        action = CaptureService.ACTION_START_AOA
        putExtra(CaptureService.EXTRA_ACCESSORY, accessory)
      }
    ContextCompat.startForegroundService(this, serviceIntent)
  }
}
