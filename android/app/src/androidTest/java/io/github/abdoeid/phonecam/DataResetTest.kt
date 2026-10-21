package io.github.abdoeid.phonecam

import android.app.ActivityManager
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Destructive only with an explicit argument; skipped by ordinary test runs. */
@RunWith(AndroidJUnit4::class)
class DataResetTest {
  @Test fun clearOwnApplicationData() {
    assumeTrue(InstrumentationRegistry.getArguments().getString("allowDataReset") == "true")
    val context = InstrumentationRegistry.getInstrumentation().targetContext
    check(context.getSystemService(ActivityManager::class.java).clearApplicationUserData())
    // Android terminates this process as part of the reset. The external runner
    // verifies that all private data disappeared rather than expecting JUnit OK.
  }
}
