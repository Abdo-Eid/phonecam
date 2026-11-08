package io.github.abdoeid.phonecam.capture

/** A waiting socket or control-only connection is not a background camera session. */
internal fun keepCaptureInBackground(
  state: CaptureState,
  hasVideoFrames: Boolean,
  controlConnected: Boolean,
  userStopped: Boolean,
  hasError: Boolean,
): Boolean =
  state == CaptureState.STREAMING && hasVideoFrames && controlConnected && !userStopped && !hasError
