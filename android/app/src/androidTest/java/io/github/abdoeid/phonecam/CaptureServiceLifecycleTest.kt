package io.github.abdoeid.phonecam

import android.app.ActivityManager
import android.app.NotificationManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.net.LocalServerSocket
import android.os.IBinder
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import io.github.abdoeid.phonecam.capture.CaptureService
import io.github.abdoeid.phonecam.capture.CaptureState
import java.net.ServerSocket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** No activity launch or injected taps: MIUI restricts both in instrumentation. */
@RunWith(AndroidJUnit4::class)
class CaptureServiceLifecycleTest {
  @Test fun idleThenUnbindExitsWithoutLatchingStop() = withDisconnectedService { service, _ ->
    service.resumeByUser()
    assertEquals(CaptureState.IDLE, service.state)
  }

  @Test fun waitingThenUnbindExitsWithoutLatchingStop() = withDisconnectedService { service, context ->
    context.startService(Intent(context, CaptureService::class.java).setAction(CaptureService.ACTION_START))
    waitUntil("Capture should wait for a host") { service.state == CaptureState.WAITING_FOR_CONNECTION }
  }

  @Test fun failedThenUnbindExitsWithoutLatchingStop() = withDisconnectedService { service, context ->
    // Occupy both video listeners so the real asynchronous open() failure reaches
    // the service's error handler. No camera, artificial error setter, or PC needed.
    LocalServerSocket("phonecam_video").use {
      ServerSocket(27183).use {
        context.startService(Intent(context, CaptureService::class.java).setAction(CaptureService.ACTION_START))
        waitUntil("Video bind failure should show the retry state") { service.lastError != null }
        assertEquals(CaptureState.IDLE, service.state)
      }
    }
  }

  private fun withDisconnectedService(block: (CaptureService, Context) -> Unit) {
    val context = InstrumentationRegistry.getInstrumentation().targetContext
    val prefs = context.getSharedPreferences("phonecam_session", 0)
    val stoppedBefore = prefs.getBoolean("stopped", false)
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
      try {
        assertTrue("Service binds", connected.await(10, TimeUnit.SECONDS))
        val bound = requireNotNull(service)
        assertEquals("Run these tests with the PC host stopped", "", bound.connectedPc)
        block(bound, context)
        assertFalse("Leaving disconnected work is not a deliberate Stop", bound.userStopped)
      } finally {
        context.unbindService(connection)
      }
      val manager = context.getSystemService(ActivityManager::class.java)
      @Suppress("DEPRECATION")
      fun running() = manager.getRunningServices(Int.MAX_VALUE).any { it.service.className == CaptureService::class.java.name }
      waitUntil("Disconnected service must exit after UI unbind") { !running() }
      waitUntil("Notification removed") { context.getSystemService(NotificationManager::class.java).activeNotifications.isEmpty() }
      assertFalse("Exit must not latch Stop", prefs.getBoolean("stopped", false))
    } finally {
      prefs.edit().putBoolean("stopped", stoppedBefore).commit()
    }
  }

  private fun waitUntil(message: String, condition: () -> Boolean) {
    val deadline = System.currentTimeMillis() + 10_000
    while (!condition() && System.currentTimeMillis() < deadline) Thread.sleep(100)
    assertTrue(message, condition())
  }

  @Test fun stopThenUnbindExitsAndRemovesNotification() {
    val context = InstrumentationRegistry.getInstrumentation().targetContext
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
      waitUntil("Stopped notification is removed") { context.getSystemService(NotificationManager::class.java).activeNotifications.isEmpty() }
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
    // This test deliberately leaves Stop latched, never resumes the camera.
    assertTrue("Stop survives exit (was $stoppedBefore)", context.getSharedPreferences("phonecam_session", 0).getBoolean("stopped", false))
  }
}
