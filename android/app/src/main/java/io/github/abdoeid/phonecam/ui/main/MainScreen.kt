package io.github.abdoeid.phonecam.ui.main

import android.Manifest
import android.content.pm.PackageManager
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

    // TEMPORARY (dev/testing only, Phase 3C): auto-starts capture whenever
    // Idle (covers first launch, and after a Stop) and auto-retries after a
    // short delay on Error (covers a PC-side disconnect mid-test) -- so a
    // rebuild+install+relaunch cycle, or recovering from a PC-side hiccup,
    // doesn't need a physical tap on this MIUI device (adb input injection
    // is blocked here -- see docs/architecture.md). Remove this effect
    // (back to manual Start/Retry-button-only) before Phase 4/release.
    LaunchedEffect(state) {
      when (state) {
        CaptureUiState.Idle -> viewModel.startCapture()
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
