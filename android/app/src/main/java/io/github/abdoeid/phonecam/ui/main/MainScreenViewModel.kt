package io.github.abdoeid.phonecam.ui.main

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import io.github.abdoeid.phonecam.capture.CaptureController
import io.github.abdoeid.phonecam.capture.CaptureState
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

sealed interface CaptureUiState {
  data object Idle : CaptureUiState

  data object WaitingForConnection : CaptureUiState

  data class Streaming(val framesEncoded: Int, val measuredFps: Double) : CaptureUiState

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
    _uiState.value = CaptureUiState.WaitingForConnection
    controller.start(width, height, fps, bitrateBps) { error ->
      statsJob?.cancel()
      controller.stop()
      _uiState.value = CaptureUiState.Error(error.message ?: error.toString())
    }
    statsJob =
      viewModelScope.launch {
        while (_uiState.value !is CaptureUiState.Error) {
          _uiState.value =
            when (controller.state) {
              CaptureState.IDLE -> CaptureUiState.Idle
              CaptureState.WAITING_FOR_CONNECTION -> CaptureUiState.WaitingForConnection
              CaptureState.STREAMING -> CaptureUiState.Streaming(controller.framesEncoded, controller.measuredFps)
            }
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
