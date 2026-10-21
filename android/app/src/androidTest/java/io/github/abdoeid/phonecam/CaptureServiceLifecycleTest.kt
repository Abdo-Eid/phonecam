package io.github.abdoeid.phonecam

import android.app.ActivityManager
import android.app.NotificationManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import io.github.abdoeid.phonecam.capture.CaptureService
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** No activity launch or injected taps: MIUI restricts both in instrumentation. */
@RunWith(AndroidJUnit4::class)
class CaptureServiceLifecycleTest {
  @Test fun stopThenUnbindExitsAndRemovesNotification() {
    val context = InstrumentationRegistry.getInstrumentation().targetContext
    val pairedBefore = context.getSharedPreferences("phonecam_trusted_pcs", 0).all
    val stoppedBefore = context.getSharedPreferences("phonecam_session", 0).getBoolean("stopped", false)
    val connected = CountDownLatch(1)
    var service: CaptureService? = null
    val connection = object : ServiceConnection {
      override fun onServiceConnected(name: ComponentName?, binder: IBinder) {
        service = (binder as CaptureService.LocalBinder).getService()
        connected.countDown()
      }
      override fun onServiceDisconnected(name: ComponentName?) = Unit
    }
    assertTrue(context.bindService(Intent(context, CaptureService::class.java), connection, Context.BIND_AUTO_CREATE))
    try {
      assertTrue("Service binds", connected.await(10, TimeUnit.SECONDS))
      service!!.pauseByUser()
      assertTrue("Stop intent is durable", context.getSharedPreferences("phonecam_session", 0).getBoolean("stopped", false))
      assertTrue("Stopped notification is removed", context.getSystemService(NotificationManager::class.java).activeNotifications.isEmpty())
    } finally {
      context.unbindService(connection)
    }
    val manager = context.getSystemService(ActivityManager::class.java)
    @Suppress("DEPRECATION")
    fun running() = manager.getRunningServices(Int.MAX_VALUE).any { it.service.className == CaptureService::class.java.name }
    val deadline = System.currentTimeMillis() + 10_000
    while (running() && System.currentTimeMillis() < deadline) Thread.sleep(100)
    assertFalse("Stopped service exits after UI unbind", running())
    assertTrue("No notification after exit", context.getSystemService(NotificationManager::class.java).activeNotifications.isEmpty())
    assertEquals("Pairing is untouched", pairedBefore, context.getSharedPreferences("phonecam_trusted_pcs", 0).all)
    // This test deliberately leaves Stop latched, never resumes the camera.
    assertTrue("Stop survives exit (was $stoppedBefore)", context.getSharedPreferences("phonecam_session", 0).getBoolean("stopped", false))
  }
}
