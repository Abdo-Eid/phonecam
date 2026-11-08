package io.github.abdoeid.phonecam.ui.main

// Keep USB connection guidance visible whenever no PC is connected, including
// while stopped or waiting for a host.
internal fun showConnectionChoices(state: CaptureUiState, connectedPc: String): Boolean =
  connectedPc.isBlank() && state !is CaptureUiState.Streaming
