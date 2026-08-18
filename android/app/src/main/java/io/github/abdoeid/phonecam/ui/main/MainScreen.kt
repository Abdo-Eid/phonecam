package io.github.abdoeid.phonecam.ui.main

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation3.runtime.NavKey
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

  // Phase 5: CaptureService's ongoing notification needs this on API 33+ to actually be visible
  // (the foreground service itself still runs without it -- this is UX polish, not a gate).
  // Requested proactively rather than tied to the camera-permission flow since it's unrelated to
  // whether capture can start at all.
  val notificationPermissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {}
  LaunchedEffect(Unit) {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
    }
  }

  Column(
    modifier = modifier.fillMaxSize().padding(16.dp),
    verticalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterVertically),
    horizontalAlignment = Alignment.CenterHorizontally,
  ) {
    Text("PhoneCam")

    if (!hasCameraPermission) {
      Text("Camera permission is required.")
      Button(onClick = { permissionLauncher.launch(Manifest.permission.CAMERA) }) { Text("Grant permission") }
      return@Column
    }

    // Auto-starts capture exactly once, on first launch, and auto-retries after a short delay on
    // Error (covers a PC-side disconnect mid-test) -- but Idle reached via a user's own Cancel/Stop
    // tap does NOT auto-restart; that's a deliberate choice, not an oversight. Added in Phase 3C
    // as dev/testing scaffolding (this MIUI device blocks adb input injection entirely, so a
    // manual tap was needed for every rebuild cycle); kept past Phase 4 on merit rather than
    // reverted mechanically -- Phase 5's planned foreground-service auto-start is the same
    // behavior made permanent, so this is a reasonable stand-in until that lands. See
    // docs/architecture.md.
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

    when (val s = state) {
      CaptureUiState.Idle -> Button(onClick = { viewModel.startCapture() }) { Text("Start capture") }
      CaptureUiState.WaitingForConnection -> {
        Text("Waiting for PC connection...")
        Button(onClick = { viewModel.stopCapture() }) { Text("Cancel") }
      }
      is CaptureUiState.Streaming -> {
        Text("Streaming to PC...")
        Text("Frames encoded: ${s.framesEncoded}")
        Text("Measured fps: ${"%.1f".format(s.measuredFps)}")
        Button(onClick = { viewModel.stopCapture() }) { Text("Stop") }
      }
      is CaptureUiState.Error -> {
        Text("Error: ${s.message}")
        Button(onClick = { viewModel.startCapture() }) { Text("Retry") }
      }
    }
  }
}
