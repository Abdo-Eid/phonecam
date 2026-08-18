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
import io.github.abdoeid.phonecam.capture.CaptureController
import io.github.abdoeid.phonecam.theme.PhoneCamTheme
import io.github.abdoeid.phonecam.transport.AoaTransport

private const val TAG = "PhoneCamAoa"

class MainActivity : ComponentActivity() {
  // TEMPORARY test wiring for the Phase 7 "real AoaTransport carries real H.264" proof (see
  // docs/architecture.md's Phase 7 section) -- starts a hardcoded test capture session straight
  // over the accessory pipe the moment it attaches, bypassing the normal Start-button/
  // CaptureService/adb-socket flow entirely. Not the final product path: the real integration
  // wires AoaTransport in as an alternative to VideoSocketServer behind that same UI flow, once
  // this proves the framing end-to-end (see CaptureController.start's videoSink parameter).
  private val aoaTransport = AoaTransport()
  private var aoaCaptureController: CaptureController? = null

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

    val ok = aoaTransport.open(usbManager, accessory)
    Log.i(TAG, "AOA transport: ${if (ok) "opened" else "FAILED"} (accessory=${accessory.model})")
    if (!ok) return

    // TEMPORARY: hardcoded test parameters, see class doc comment.
    val controller = CaptureController(applicationContext)
    aoaCaptureController = controller
    controller.start(
      width = 1280,
      height = 720,
      fps = 30,
      bitrateBps = 4_000_000,
      onError = { e -> Log.e(TAG, "AOA test capture error", e) },
      videoSink = aoaTransport.videoOutput,
    )
  }

  override fun onDestroy() {
    super.onDestroy()
    aoaCaptureController?.stop()
    aoaTransport.close()
  }
}
