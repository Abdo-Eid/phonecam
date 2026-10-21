package io.github.abdoeid.phonecam

import android.app.ActivityManager
import android.app.NotificationManager
import androidx.lifecycle.Lifecycle
import androidx.test.platform.app.InstrumentationRegistry
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Live-device tests for the connection UX. A paired tray host must be running on
 * the same network. These never clear pairing keys or app data.
 */
@RunWith(AndroidJUnit4::class)
class ConnectionUxTest {
  @get:Rule val compose = createAndroidComposeRule<MainActivity>()

  private fun await(label: String) {
    compose.waitUntil(45_000) { compose.onAllNodesWithText(label).fetchSemanticsNodes().isNotEmpty() }
    compose.onNodeWithText(label).assertIsDisplayed()
  }

  private fun present(label: String) =
    compose.onAllNodesWithText(label).fetchSemanticsNodes().isNotEmpty()

  @Test fun stopStaysStoppedAndHidesPairing() {
    // A previous deliberate Stop is allowed; only an explicit button press resumes it.
    compose.waitUntil(45_000) { present("START") || present("STOP") }
    if (present("START")) compose.onNodeWithText("START").performClick()
    await("STOP") // cancelling Connecting must latch Stop too, even before the first frame
    compose.onNodeWithText("STOP").performClick()
    await("STOPPED")
    compose.onNodeWithText("SCAN PC QR").assertDoesNotExist()
    compose.onNodeWithText("PAIRING CODE").assertDoesNotExist()
    compose.onNodeWithText("CONNECTION SETTINGS").performClick()
    compose.onNodeWithText("Connection settings").assertIsDisplayed()
    compose.onNodeWithText("PAIRING CODE").assertIsDisplayed()
    compose.onNodeWithText("Done").performClick()
    // Four watchdog intervals, not merely a transient UI assertion.
    Thread.sleep(12_000)
    compose.onNodeWithText("STOPPED").assertIsDisplayed()
    compose.onNodeWithText("START").assertIsDisplayed()
    compose.activityRule.scenario.recreate()
    await("STOPPED")
  }

  @Test fun explicitStartResumesRememberedConnection() {
    await("STOPPED")
    compose.onNodeWithText("START").performClick()
    await("STREAMING")
    compose.onNodeWithText("SCAN PC QR").assertDoesNotExist()
    compose.onNodeWithText("PAIRING CODE").assertDoesNotExist()
    compose.onNodeWithText("STOP").performClick()
    await("STOPPED")
    Thread.sleep(12_000)
    compose.onNodeWithText("STOPPED").assertIsDisplayed()
    compose.onNodeWithText("START").performClick()
    await("STREAMING")
  }

  @Test fun stoppedAppExitsWithoutAnOngoingNotification() {
    compose.waitUntil(45_000) { present("START") || present("STOP") }
    if (present("STOP")) compose.onNodeWithText("STOP").performClick()
    // Ready -> Start -> Stop also works with no laptop running.
    if (!present("STOPPED")) {
      compose.onNodeWithText("START").performClick()
      await("STOP")
      compose.onNodeWithText("STOP").performClick()
    }
    await("STOPPED")
    val context = InstrumentationRegistry.getInstrumentation().targetContext
    val notifications = context.getSystemService(NotificationManager::class.java)
    val activityManager = context.getSystemService(ActivityManager::class.java)
    val pcs = context.getSharedPreferences("phonecam_trusted_pcs", 0).all
    compose.activityRule.scenario.moveToState(Lifecycle.State.CREATED)
    val deadline = System.currentTimeMillis() + 10_000
    @Suppress("DEPRECATION")
    fun serviceRunning() = activityManager.getRunningServices(Int.MAX_VALUE).any {
      it.service.className == "io.github.abdoeid.phonecam.capture.CaptureService"
    }
    while (serviceRunning() && System.currentTimeMillis() < deadline) Thread.sleep(100)
    org.junit.Assert.assertFalse("Stopped service must exit when UI closes", serviceRunning())
    org.junit.Assert.assertTrue("No ongoing PhoneCam notification", notifications.activeNotifications.isEmpty())
    org.junit.Assert.assertEquals("Pairing is preserved", pcs, context.getSharedPreferences("phonecam_trusted_pcs", 0).all)
    compose.activityRule.scenario.moveToState(Lifecycle.State.RESUMED)
    await("STOPPED")
  }
}
