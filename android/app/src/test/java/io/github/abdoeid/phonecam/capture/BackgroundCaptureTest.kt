package io.github.abdoeid.phonecam.capture

import org.junit.Assert.assertEquals
import org.junit.Test

class BackgroundCaptureTest {
  @Test fun onlyConnectedStreamingMayOutliveTheUi() {
    for (state in CaptureState.entries) {
      for (frames in listOf(false, true)) {
        for (connected in listOf(false, true)) {
          for (stopped in listOf(false, true)) {
            for (error in listOf(false, true)) {
              val expected = state == CaptureState.STREAMING && frames && connected && !stopped && !error
              assertEquals(
                "$state frames=$frames connected=$connected stopped=$stopped error=$error",
                expected, keepCaptureInBackground(state, frames, connected, stopped, error),
              )
            }
          }
        }
      }
    }
  }
}
