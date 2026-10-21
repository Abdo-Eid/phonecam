package io.github.abdoeid.phonecam.ui.main

import org.junit.Assert.*
import org.junit.Test

class ConnectionChoicesTest {
  @Test fun disconnectedStatesAlwaysOfferConnectionActions() {
    for (state in listOf(CaptureUiState.Idle, CaptureUiState.Stopped,
      CaptureUiState.WaitingForConnection, CaptureUiState.Error("No PC"))) {
      assertTrue("$state needs a direct connection action", showConnectionChoices(state, ""))
    }
  }

  @Test fun connectedAndStreamingStatesKeepActionsInSettings() {
    for (state in listOf(CaptureUiState.Idle, CaptureUiState.Stopped,
      CaptureUiState.WaitingForConnection, CaptureUiState.Streaming(1.0, 30.0))) {
      assertFalse(showConnectionChoices(state, "My PC"))
    }
    assertFalse(showConnectionChoices(CaptureUiState.Streaming(1.0, 30.0), ""))
  }
}
