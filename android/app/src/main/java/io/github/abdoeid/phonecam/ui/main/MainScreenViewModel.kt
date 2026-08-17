package io.github.abdoeid.phonecam.ui.main

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import io.github.abdoeid.phonecam.capture.CaptureController
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

sealed interface CaptureUiState {
  data object Idle : CaptureUiState

  data class Recording(val framesEncoded: Int, val measuredFps: Double, val outputPath: String) : CaptureUiState

  data class Error(val message: String) : CaptureUiState
}

class MainScreenViewModel(application: Application) : AndroidViewModel(application) {
  private val controller = CaptureController(application)
  private val _uiState = MutableStateFlow<CaptureUiState>(CaptureUiState.Idle)
  val uiState: StateFlow<CaptureUiState> = _uiState.asStateFlow()

  private var statsJob: Job? = null

  // TODO(Phase 4): resolution/fps become PC-driven via the control channel.
  // 720p is the default here because it's the more thoroughly measured of
  // the two Phase 2 exit-criterion datapoints (~28-30fps sustained, vs. only
  // a frame count for the 1080p run) -- see docs/architecture.md.
  fun startCapture(width: Int = 1280, height: Int = 720, fps: Int = 30, bitrateBps: Int = 4_000_000) {
    controller.start(width, height, fps, bitrateBps) { error ->
      statsJob?.cancel()
      _uiState.value = CaptureUiState.Error(error.message ?: error.toString())
    }
    statsJob =
      viewModelScope.launch {
        while (_uiState.value !is CaptureUiState.Error) {
          val path = controller.outputFile?.absolutePath ?: ""
          _uiState.value = CaptureUiState.Recording(controller.framesEncoded, controller.measuredFps, path)
          delay(500)
        }
      }
  }

  fun stopCapture() {
    statsJob?.cancel()
    statsJob = null
    controller.stop()
    _uiState.value = CaptureUiState.Idle
  }

  override fun onCleared() {
    super.onCleared()
    controller.stop()
  }
}
