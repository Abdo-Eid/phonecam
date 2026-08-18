package io.github.abdoeid.phonecam.ui.main

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.BorderStroke
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.SignalWifiOff
import androidx.compose.material.icons.filled.VideocamOff
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation3.runtime.NavKey
import io.github.abdoeid.phonecam.theme.Dim
import io.github.abdoeid.phonecam.theme.Ink
import io.github.abdoeid.phonecam.theme.NoSignalBlue
import io.github.abdoeid.phonecam.theme.Paper
import io.github.abdoeid.phonecam.theme.Panel as PanelColor
import io.github.abdoeid.phonecam.theme.PanelLine
import io.github.abdoeid.phonecam.theme.StandbyAmber
import io.github.abdoeid.phonecam.theme.StandbyDim
import io.github.abdoeid.phonecam.theme.TallyRed
import kotlinx.coroutines.delay

@Composable
fun MainScreen(onItemClick: (NavKey) -> Unit, modifier: Modifier = Modifier, viewModel: MainScreenViewModel = viewModel()) {
  val state by viewModel.uiState.collectAsStateWithLifecycle()
  val context = LocalContext.current
  var hasCameraPermission by remember {
    mutableStateOf(
      ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
    )
  }
  val permissionLauncher =
    rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted -> hasCameraPermission = granted }

  val notificationPermissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {}
  LaunchedEffect(Unit) {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
    }
  }

  var hasAutoStarted by remember { mutableStateOf(false) }
  LaunchedEffect(state) {
    when (state) {
      CaptureUiState.Idle ->
        if (!hasAutoStarted) {
          hasAutoStarted = true
          viewModel.startCapture()
        }
      is CaptureUiState.Error -> {
        delay(1500)
        viewModel.startCapture()
      }
      else -> {}
    }
  }

  Column(
    modifier = modifier.fillMaxSize().background(Ink).padding(horizontal = 28.dp, vertical = 36.dp),
    horizontalAlignment = Alignment.CenterHorizontally,
  ) {
    Nameplate(state = state)

    Spacer(Modifier.weight(1f))

    if (!hasCameraPermission) {
      PermissionPanel(onGrant = { permissionLauncher.launch(Manifest.permission.CAMERA) })
    } else {
      ViewfinderPanel(state = state, onStart = { viewModel.startCapture() }, onStop = { viewModel.stopCapture() })
    }

    Spacer(Modifier.weight(1f))

    Text(
      "connects automatically when phonecam.exe is running on your pc",
      style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace, fontSize = 11.sp),
      color = Dim,
      textAlign = TextAlign.Center,
    )
  }
}

// Wordmark + a live status dot -- a device nameplate, not an app-icon badge. The dot is the
// same accent StatusVisual would give the current state, so the header itself hints at state
// even before you look at the panel below.
@Composable
private fun Nameplate(state: CaptureUiState) {
  val accent by animateColorAsState(statusVisualFor(state).accent, label = "nameplateAccent")
  Column(horizontalAlignment = Alignment.Start, modifier = Modifier.fillMaxWidth()) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
      Box(modifier = Modifier.size(9.dp).background(accent, CircleShape))
      Text(
        "PHONECAM",
        style = MaterialTheme.typography.headlineSmall,
        fontWeight = FontWeight.Black,
        letterSpacing = 3.sp,
        color = Paper,
      )
    }
    Text(
      "usb → windows  //  virtual camera",
      style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace, fontSize = 12.sp),
      color = Dim,
      modifier = Modifier.padding(start = 19.dp, top = 2.dp),
    )
  }
}

@Composable
private fun PermissionPanel(onGrant: () -> Unit) {
  Panel {
    Column(
      modifier = Modifier.padding(28.dp),
      horizontalAlignment = Alignment.CenterHorizontally,
      verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
      Icon(Icons.Filled.VideocamOff, contentDescription = null, tint = Dim, modifier = Modifier.size(36.dp))
      Text(
        "CAMERA ACCESS REQUIRED",
        style = MaterialTheme.typography.titleMedium,
        fontWeight = FontWeight.Bold,
        letterSpacing = 1.sp,
        color = Paper,
        textAlign = TextAlign.Center,
      )
      Text(
        "PhoneCam needs the camera to send a signal to your PC.",
        style = MaterialTheme.typography.bodyMedium,
        color = Dim,
        textAlign = TextAlign.Center,
      )
      OutlinedButton(
        onClick = onGrant,
        shape = RoundedCornerShape(2.dp),
        border = BorderStroke(1.dp, PanelLine),
      ) {
        Text("GRANT ACCESS", letterSpacing = 1.sp)
      }
    }
  }
}

// The shared panel chrome: sharp corners and a hairline border instead of a heavily-rounded
// filled Material card -- reads as an instrument housing, not a consumer app surface.
@Composable
private fun Panel(content: @Composable () -> Unit) {
  Box(
    modifier =
      Modifier.fillMaxWidth()
        .background(PanelColor, RoundedCornerShape(4.dp))
        .border(1.dp, PanelLine, RoundedCornerShape(4.dp)),
  ) {
    content()
  }
}

private data class StatusVisual(val accent: Color, val label: String)

private fun statusVisualFor(state: CaptureUiState): StatusVisual =
  when (state) {
    CaptureUiState.Idle -> StatusVisual(StandbyDim, "STANDBY")
    CaptureUiState.WaitingForConnection -> StatusVisual(StandbyAmber, "SEARCHING")
    is CaptureUiState.Streaming -> StatusVisual(TallyRed, "ON AIR")
    is CaptureUiState.Error -> StatusVisual(NoSignalBlue, "NO SIGNAL")
  }

@Composable
private fun ViewfinderPanel(state: CaptureUiState, onStart: () -> Unit, onStop: () -> Unit) {
  val visual = statusVisualFor(state)
  val accent by animateColorAsState(visual.accent, label = "panelAccent")

  Panel {
    Column(
      modifier = Modifier.padding(vertical = 32.dp, horizontal = 24.dp),
      horizontalAlignment = Alignment.CenterHorizontally,
      verticalArrangement = Arrangement.spacedBy(22.dp),
    ) {
      ViewfinderBrackets(accent = accent, size = 92.dp) {
        when (state) {
          CaptureUiState.WaitingForConnection -> ScanningGlyph(accent)
          is CaptureUiState.Error -> Icon(Icons.Filled.SignalWifiOff, contentDescription = null, tint = accent, modifier = Modifier.size(30.dp))
          else -> PulsingDot(color = accent, active = state is CaptureUiState.Streaming, size = 16.dp)
        }
      }

      Text(
        visual.label,
        style = MaterialTheme.typography.titleLarge,
        fontWeight = FontWeight.Black,
        letterSpacing = 3.sp,
        color = accent,
      )

      AnimatedContent(
        targetState = state,
        transitionSpec = { fadeIn(tween(220)) togetherWith fadeOut(tween(140)) },
        label = "panelDetail",
      ) { s ->
        when (s) {
          CaptureUiState.Idle ->
            MonoCaption("tap start to begin transmitting")
          CaptureUiState.WaitingForConnection ->
            MonoCaption("open phonecam on your pc")
          is CaptureUiState.Streaming -> OsdReadout(s.elapsedSeconds, s.measuredFps)
          is CaptureUiState.Error -> MonoCaption(s.message, color = Dim)
        }
      }

      when (state) {
        CaptureUiState.Idle -> PanelButton("START", TallyRed, onStart)
        CaptureUiState.WaitingForConnection -> PanelButton("CANCEL", PanelLine, onStop, filled = false)
        is CaptureUiState.Streaming -> PanelButton("STOP", PanelLine, onStop, filled = false)
        is CaptureUiState.Error -> PanelButton("RETRY NOW", NoSignalBlue, onStart)
      }
    }
  }
}

@Composable
private fun MonoCaption(text: String, color: Color = Dim) {
  Text(
    text,
    style = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace, fontSize = 13.sp),
    color = color,
    textAlign = TextAlign.Center,
  )
}

@Composable
private fun PanelButton(label: String, accent: Color, onClick: () -> Unit, filled: Boolean = true) {
  if (filled) {
    Button(
      onClick = onClick,
      shape = RoundedCornerShape(2.dp),
      colors = ButtonDefaults.buttonColors(containerColor = accent, contentColor = Ink),
    ) {
      Text(label, fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
    }
  } else {
    OutlinedButton(
      onClick = onClick,
      shape = RoundedCornerShape(2.dp),
      border = BorderStroke(1.dp, PanelLine),
      colors = ButtonDefaults.outlinedButtonColors(contentColor = Paper),
    ) {
      Text(label, fontWeight = FontWeight.Medium, letterSpacing = 1.sp)
    }
  }
}

// A real camera's on-screen viewfinder display: dim tabular labels, bright monospace values.
// FPS comes straight from measured encoder output; RES reflects Phase 6's fixed 1080p pipeline
// (see docs/architecture.md) -- would need to become dynamic alongside a future PC-driven
// SetResolution. TIME is elapsed mm:ss rather than a raw frame count -- a frame counter's digit
// width keeps growing for as long as a session runs, while mm:ss stays a fixed, glanceable shape
// (and rolls over to hh:mm:ss only past a full hour, far longer than any real session).
@Composable
private fun OsdReadout(elapsedSeconds: Double, measuredFps: Double) {
  Row(horizontalArrangement = Arrangement.spacedBy(18.dp), verticalAlignment = Alignment.CenterVertically) {
    OsdField("FPS", "%.1f".format(measuredFps))
    OsdDivider()
    OsdField("RES", "1920×1080")
    OsdDivider()
    OsdField("TIME", formatElapsed(elapsedSeconds))
  }
}

private fun formatElapsed(seconds: Double): String {
  val total = seconds.toLong().coerceAtLeast(0)
  val h = total / 3600
  val m = (total % 3600) / 60
  val s = total % 60
  return if (h > 0) "%d:%02d:%02d".format(h, m, s) else "%02d:%02d".format(m, s)
}

@Composable
private fun OsdField(label: String, value: String) {
  Column(horizontalAlignment = Alignment.CenterHorizontally) {
    Text(value, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold, fontSize = 15.sp, color = Paper)
    Text(label, fontFamily = FontFamily.Monospace, fontSize = 10.sp, letterSpacing = 1.sp, color = Dim)
  }
}

@Composable
private fun OsdDivider() {
  Box(modifier = Modifier.width(1.dp).height(24.dp).background(PanelLine))
}

// The signature motif: camera-viewfinder corner brackets, not a filled icon badge. Reused across
// every state so the state-specific glyph inside (a pulsing dot, a scan sweep, a no-signal icon)
// always reads as "what the viewfinder is showing right now."
@Composable
private fun ViewfinderBrackets(accent: Color, size: Dp, content: @Composable BoxScope.() -> Unit) {
  Box(modifier = Modifier.size(size), contentAlignment = Alignment.Center) {
    Canvas(modifier = Modifier.fillMaxSize()) {
      val bracket = this.size.minDimension * 0.22f
      val stroke = Stroke(width = 2.5.dp.toPx())
      val w = this.size.width
      val h = this.size.height
      val corners =
        listOf(
          Offset(0f, 0f) to Offset(1f, 1f),
          Offset(w, 0f) to Offset(-1f, 1f),
          Offset(0f, h) to Offset(1f, -1f),
          Offset(w, h) to Offset(-1f, -1f),
        )
      for ((origin, dir) in corners) {
        drawLine(accent, origin, Offset(origin.x + bracket * dir.x, origin.y), stroke.width)
        drawLine(accent, origin, Offset(origin.x, origin.y + bracket * dir.y), stroke.width)
      }
    }
    content()
  }
}

@Composable
private fun PulsingDot(color: Color, active: Boolean, size: Dp) {
  val transition = rememberInfiniteTransition(label = "pulse")
  val alpha by
    transition.animateFloat(
      initialValue = if (active) 0.35f else 1f,
      targetValue = 1f,
      animationSpec = infiniteRepeatable(tween(700, easing = LinearEasing), RepeatMode.Reverse),
      label = "pulseAlpha",
    )
  Box(modifier = Modifier.size(size).background(color.copy(alpha = if (active) alpha else 1f), CircleShape))
}

// A horizontal sweep, evoking a signal search / tuning scan rather than a generic spinner.
@Composable
private fun ScanningGlyph(color: Color) {
  val transition = rememberInfiniteTransition(label = "scan")
  val x by
    transition.animateFloat(
      initialValue = -1f,
      targetValue = 1f,
      animationSpec = infiniteRepeatable(tween(900, easing = LinearEasing), RepeatMode.Restart),
      label = "scanX",
    )
  Canvas(modifier = Modifier.size(40.dp, 24.dp)) {
    val cx = size.width / 2f * (1 + x)
    drawLine(color.copy(alpha = 0.25f), Offset(0f, size.height / 2f), Offset(size.width, size.height / 2f), 1.5.dp.toPx())
    drawLine(color, Offset(cx, 0f), Offset(cx, size.height), 3.dp.toPx())
  }
}
