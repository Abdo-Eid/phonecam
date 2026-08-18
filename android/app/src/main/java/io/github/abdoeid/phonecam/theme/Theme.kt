package io.github.abdoeid.phonecam.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

// Always dark, regardless of system theme -- a deliberate choice, not an oversight. PhoneCam's
// identity is a monitor/viewfinder housing (see Color.kt); a light variant would break that
// metaphor entirely, the same reason most video/audio production tools force a dark UI
// regardless of the OS setting. Dynamic color (Material You) is also deliberately not used here
// -- it would replace this palette with one derived from the user's wallpaper, defeating the
// point of a specific, chosen identity.
private val PhoneCamColorScheme =
  darkColorScheme(
    primary = TallyRed,
    onPrimary = Paper,
    primaryContainer = Panel,
    onPrimaryContainer = Paper,
    secondary = StandbyAmber,
    tertiary = NoSignalBlue,
    background = Ink,
    onBackground = Paper,
    surface = Panel,
    onSurface = Paper,
    surfaceVariant = Panel,
    onSurfaceVariant = Dim,
    outline = PanelLine,
  )

@Composable fun PhoneCamTheme(content: @Composable () -> Unit) {
  MaterialTheme(colorScheme = PhoneCamColorScheme, typography = Typography, content = content)
}
