package io.github.abdoeid.phonecam.ui.main

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.widget.Toast
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
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.material3.TextButton
import androidx.compose.material3.AlertDialog
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
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
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation3.runtime.NavKey
import io.github.abdoeid.phonecam.control.PairingCode
import io.github.abdoeid.phonecam.theme.Dim
import io.github.abdoeid.phonecam.theme.Ink
import io.github.abdoeid.phonecam.theme.NoSignalBlue
import io.github.abdoeid.phonecam.theme.Paper
import io.github.abdoeid.phonecam.theme.Panel as PanelColor
import io.github.abdoeid.phonecam.theme.PanelLine
import io.github.abdoeid.phonecam.theme.StandbyAmber
import io.github.abdoeid.phonecam.theme.StandbyDim
import io.github.abdoeid.phonecam.theme.TallyRed
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions

@Composable
fun MainScreen(onItemClick: (NavKey) -> Unit, modifier: Modifier = Modifier, viewModel: MainScreenViewModel = viewModel()) {
  val state by viewModel.uiState.collectAsStateWithLifecycle()
  val context = LocalContext.current
  val lifecycleOwner = LocalLifecycleOwner.current
  DisposableEffect(lifecycleOwner, viewModel) {
    val observer = LifecycleEventObserver { _, event ->
      when (event) {
        Lifecycle.Event.ON_START -> viewModel.bindUi()
        Lifecycle.Event.ON_STOP -> viewModel.unbindUi()
        else -> Unit
      }
    }
    lifecycleOwner.lifecycle.addObserver(observer)
    onDispose {
      lifecycleOwner.lifecycle.removeObserver(observer)
      viewModel.unbindUi()
    }
  }
  var hasCameraPermission by remember {
    mutableStateOf(
      ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
    )
  }
  var startUsbAfterPermission by remember { mutableStateOf(false) }
  val permissionLauncher =
    rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
      hasCameraPermission = granted
      if (granted && startUsbAfterPermission) viewModel.startCapture()
      startUsbAfterPermission = false
    }

  val notificationPermissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {}
  LaunchedEffect(Unit) {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
    }
  }

  // The pairing code: what a PC has to be given before it may drive this camera over a
  // network link. Shown here because the PC asks for it, and there is no other way to get
  // it onto the other machine.
  val pairing = remember { PairingCode(context) }
  var pairingCode by remember { mutableStateOf(pairing.code) }
  val pairedPcs by viewModel.pairedPcs.collectAsStateWithLifecycle()
  val pairingMessage by viewModel.pairingMessage.collectAsStateWithLifecycle()
  val connectedPc by viewModel.connectedPc.collectAsStateWithLifecycle()
  var showSettings by remember { mutableStateOf(false) }
  var showManualCode by remember { mutableStateOf(false) }
  var forgetId by remember { mutableStateOf<String?>(null) }
  val qrScanner = rememberLauncherForActivityResult(ScanContract()) {
    hasCameraPermission = ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
    viewModel.qrScanned(it.contents)
  }

  LaunchedEffect(viewModel) {
    viewModel.notifications.collect { Toast.makeText(context, it, Toast.LENGTH_SHORT).show() }
  }
  val streaming = state is CaptureUiState.Streaming
  LaunchedEffect(streaming) {
    if (streaming) { showSettings = false; showManualCode = false }
  }
  val scanQr = {
    if (viewModel.beginQrScan()) {
      showSettings = false
      qrScanner.launch(ScanOptions().setDesiredBarcodeFormats(ScanOptions.QR_CODE)
        .setPrompt("Scan the QR in PhoneCam on your PC").setBeepEnabled(false)
        .setOrientationLocked(false).setCaptureActivity(QrScanActivity::class.java))
    }
  }

  Column(
    modifier = modifier.fillMaxSize().background(Ink).padding(horizontal = 28.dp, vertical = 36.dp),
    horizontalAlignment = Alignment.CenterHorizontally,
  ) {
    Nameplate(state = state)
    if (connectedPc.isNotEmpty()) {
      Text(connectedPc, color = Paper, modifier = Modifier.padding(top = 12.dp))
    } else if (pairedPcs.isNotEmpty()) {
      Text("Remembered: ${pairedPcs.joinToString { it.name }}", color = Dim, modifier = Modifier.padding(top = 12.dp))
    }

    Spacer(Modifier.weight(1f))

    if (!hasCameraPermission) {
      PermissionPanel(onGrant = { permissionLauncher.launch(Manifest.permission.CAMERA) })
    } else {
      ViewfinderPanel(state = state, onStart = { viewModel.startCapture() }, onStop = { viewModel.stopCapture() })
    }

    Spacer(Modifier.weight(1f))

    if (showConnectionChoices(state, connectedPc)) {
      Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text("CONNECT TO YOUR PC", color = Paper, style = MaterialTheme.typography.labelMedium)
        TextButton(onClick = scanQr) { Text("SCAN PC QR", color = NoSignalBlue) }
        TextButton(onClick = { showManualCode = true }) { Text("USE PAIRING CODE", color = Paper) }
        TextButton(onClick = {
          if (hasCameraPermission) viewModel.startCapture() else {
            startUsbAfterPermission = true
            permissionLauncher.launch(Manifest.permission.CAMERA)
          }
        }) { Text("CONNECT WITH USB", color = Paper) }
        Text("Cable connected? Open the PC tray → Use USB.", color = Dim,
          style = MaterialTheme.typography.bodySmall, textAlign = TextAlign.Center)
      }
    }
    if (pairingMessage.isNotEmpty() && !streaming && state != CaptureUiState.Stopped) {
      Text(pairingMessage, color = Dim, style = MaterialTheme.typography.bodySmall)
    }
    TextButton(onClick = { showSettings = true }) { Text("CONNECTION SETTINGS", color = Dim) }

    Text(
      "Wi-Fi / USB → Windows virtual camera",
      style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace, fontSize = 11.sp),
      color = Dim,
      textAlign = TextAlign.Center,
    )
  }
  if (showManualCode) {
    AlertDialog(
      onDismissRequest = { showManualCode = false },
      title = { Text("Connect with a code") },
      text = {
        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
          Text("On the PC tray: Connect over Wi-Fi → Use code instead. Enter this phone's code there.")
          PairingCodeRow(pairingCode) { pairingCode = pairing.regenerate() }
          Text("Both devices must be on the same Wi-Fi or hotspot.", style = MaterialTheme.typography.bodySmall)
        }
      },
      confirmButton = { TextButton(onClick = { showManualCode = false }) { Text("Done") } },
    )
  }
  if (showSettings) {
    AlertDialog(
      onDismissRequest = { showSettings = false },
      title = { Text("Connection settings") },
      text = {
        Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
          TextButton(onClick = scanQr) { Text("Scan QR / change PC") }
          Text("Manual pairing code", style = MaterialTheme.typography.labelMedium)
          PairingCodeRow(pairingCode) { pairingCode = pairing.regenerate() }
          for (pc in pairedPcs) {
            Text(pc.name)
            TextButton(onClick = { forgetId = pc.id }) { Text("Forget PC") }
          }
          Text("Video is not encrypted. Use a trusted Wi-Fi or hotspot.", style = MaterialTheme.typography.bodySmall)
        }
      },
      confirmButton = { TextButton(onClick = { showSettings = false }) { Text("Done") } },
    )
  }
  forgetId?.let { id ->
    AlertDialog(
      onDismissRequest = { forgetId = null },
      title = { Text("Forget this PC?") },
      text = { Text("It will need a new QR scan to connect again.") },
      confirmButton = { TextButton(onClick = { viewModel.forgetPc(id); forgetId = null }) { Text("Forget") } },
      dismissButton = { TextButton(onClick = { forgetId = null }) { Text("Cancel") } },
    )
  }
}

// The pairing code, grouped for reading. A PC needs it to be allowed to drive this camera
// over a network link; over USB the connection is trusted without one.
@Composable
private fun PairingCodeRow(code: String, onRegenerate: () -> Unit) {
  Row(
    verticalAlignment = Alignment.CenterVertically,
    horizontalArrangement = Arrangement.spacedBy(10.dp),
    modifier = Modifier.padding(bottom = 14.dp),
  ) {
    Text(
      "PAIRING CODE",
      style = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace, fontSize = 10.sp),
      color = Dim,
    )
    Text(
      formatPairingCode(code),
      style = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace, fontSize = 13.sp),
      color = Paper,
    )
    Text(
      "NEW",
      style = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace, fontSize = 10.sp),
      color = NoSignalBlue,
      modifier = Modifier.clickable(onClick = onRegenerate),
    )
  }
}

// "ABCD-EFGH-IJKL" -- the code is 12 characters, split into three groups so it can be read
// out loud over a phone call without confusion.
private fun formatPairingCode(code: String): String =
  code.chunked(4).joinToString("-")

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
      "wi-fi / usb → windows",
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
    CaptureUiState.Idle -> StatusVisual(StandbyDim, "READY")
    CaptureUiState.WaitingForConnection -> StatusVisual(StandbyAmber, "CONNECTING")
    is CaptureUiState.Streaming -> StatusVisual(TallyRed, "STREAMING")
    CaptureUiState.Stopped -> StatusVisual(StandbyDim, "STOPPED")
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

      // contentKey keeps the fade for genuine screen changes only. Streaming carries live
      // stats and is re-emitted every 500ms by the ViewModel's poll loop, so keying on the
      // whole state would restart this cross-fade twice a second -- a permanent flicker on the
      // OSD row instead of a transition. With the state *kind* as the key, the numbers update
      // in place and only Idle/Searching/Streaming/Error boundaries animate.
      AnimatedContent(
        targetState = state,
        contentKey = { it::class },
        transitionSpec = { fadeIn(tween(220)) togetherWith fadeOut(tween(140)) },
        label = "panelDetail",
      ) { s ->
        when (s) {
          CaptureUiState.Idle ->
            MonoCaption("open PhoneCam on your PC, or tap Start for USB")
          CaptureUiState.Stopped -> MonoCaption("camera off · tap Start to resume")
          CaptureUiState.WaitingForConnection ->
            MonoCaption("open phonecam on your pc")
          is CaptureUiState.Streaming -> OsdReadout(s.elapsedSeconds, s.measuredFps)
          is CaptureUiState.Error -> MonoCaption(s.message, color = Dim)
        }
      }

      when (state) {
        CaptureUiState.Idle -> PanelButton("START", TallyRed, onStart)
        CaptureUiState.Stopped -> PanelButton("START", TallyRed, onStart)
        CaptureUiState.WaitingForConnection -> PanelButton("STOP", PanelLine, onStop, filled = false)
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
// FPS comes straight from measured encoder output; RES reflects the fixed 1080p pipeline
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
