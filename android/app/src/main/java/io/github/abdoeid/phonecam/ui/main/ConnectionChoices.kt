package io.github.abdoeid.phonecam.ui.main

// Remembered pairing is not a live connection. Always offer a way forward when
// no PC is connected, including Stopped, a retry, or an abandoned USB wait.
internal fun showConnectionChoices(state: CaptureUiState, connectedPc: String): Boolean =
  connectedPc.isBlank() && state !is CaptureUiState.Streaming
