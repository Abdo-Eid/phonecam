package io.github.abdoeid.phonecam.theme

import androidx.compose.ui.graphics.Color

// Phase 6 UI polish, second pass -- a deliberate identity built on broadcast/viewfinder
// language rather than generic Material dark-mode defaults. PhoneCam's entire job is turning a
// phone's camera into a signal a PC receives, so the palette borrows from what that equipment
// actually looks like: a dark monitor housing, a tally light, a standby indicator, and the
// "blue screen of no signal" every CRT monitor showed when its input dropped -- an authentic,
// recognizable convention for "disconnected," not an arbitrary error color.

// Base panel -- a near-black with a cool green-teal cast (a monitor housing, not pure #000).
val Ink = Color(0xFF0B0F0E)
val Panel = Color(0xFF131A18)
val PanelLine = Color(0xFF26332F)
val Paper = Color(0xFFE9F1EE)
val Dim = Color(0xFF71857F)

// State colors -- deliberately NOT the Material red=bad/green=good convention. Tally red means
// "live," inverted from the usual "red=error," because that's what a camera's own tally light
// means; a no-signal blue (not red) marks a lost/errored connection, matching the CRT
// "no signal" blue screen instead of an arbitrary alert color.
val TallyRed = Color(0xFFFF3B4E) // Streaming / on-air
val StandbyAmber = Color(0xFFF5A623) // Waiting for PC / connecting
val NoSignalBlue = Color(0xFF3D8BFF) // Error / disconnected
val StandbyDim = Dim // Idle
