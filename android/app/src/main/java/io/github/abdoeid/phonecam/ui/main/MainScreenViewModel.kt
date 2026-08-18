package io.github.abdoeid.phonecam.ui.main

import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.hardware.camera2.CameraCharacteristics
import android.os.IBinder
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import io.github.abdoeid.phonecam.capture.CaptureService
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

  data class Streaming(val elapsedSeconds: Double, val measuredFps: Double) : CaptureUiState

  data class Error(val message: String) : CaptureUiState
}

// Phase 5: capture itself now lives in CaptureService (see its class doc), not here -- this
// ViewModel only sends start/stop intents and polls the bound service's state. Binding (not
// starting) in init{} so state can be read even if the service was already running from a prior
// Activity instance; ACTION_START/ACTION_STOP intents (not direct method calls on the bound
// service) own the actual start/stop, since that's the pattern startForegroundService() requires
// -- the service must reach startForeground() from within its own onStartCommand.
//
// Phase 7: the poll loop now runs continuously from init{} rather than being started/cancelled
// alongside startCapture()/stopCapture() -- CaptureService can also be started by MainActivity's
// AOA accessory-attach handling (ACTION_START_AOA), entirely independent of this ViewModel and
// its Start button, so uiState needs to reflect the service's real state regardless of what
// triggered it. Previously, an AOA-triggered session would stream successfully while this
// ViewModel kept reporting stale Idle indefinitely, since nothing had ever called startCapture()
// to kick off polling.
class MainScreenViewModel(application: Application) : AndroidViewModel(application) {
  private val _uiState = MutableStateFlow<CaptureUiState>(CaptureUiState.Idle)
  val uiState: StateFlow<CaptureUiState> = _uiState.asStateFlow()

  private var boundService: CaptureService? = null
  private var isBound = false
  private var statsJob: Job? = null

  private val connection =
    object : ServiceConnection {
      override fun onServiceConnected(name: ComponentName?, binder: IBinder) {
        boundService = (binder as CaptureService.LocalBinder).getService()
      }

      override fun onServiceDisconnected(name: ComponentName?) {
        boundService = null
      }
    }

  init {
    val context = getApplication<Application>()
    isBound = context.bindService(Intent(context, CaptureService::class.java), connection, Context.BIND_AUTO_CREATE)
    statsJob =
      viewModelScope.launch {
        while (true) {
          val service = boundService
          if (service == null) {
            // Bind hasn't completed yet -- init{} kicked it off, but onServiceConnected is async.
            delay(100)
            continue
          }
          val error = service.lastError
          if (error != null) {
            _uiState.value = CaptureUiState.Error(error.message ?: error.toString())
          } else {
            _uiState.value =
              when (service.state) {
                CaptureState.IDLE -> CaptureUiState.Idle
                CaptureState.WAITING_FOR_CONNECTION -> CaptureUiState.WaitingForConnection
                CaptureState.STREAMING -> CaptureUiState.Streaming(service.elapsedSeconds, service.measuredFps)
              }
          }
          delay(500)
        }
      }
  }

  // TODO(Phase 7+): resolution/fps become PC-driven SetResolution/SetFps commands once the vcam
  // supports a non-fixed stream size (docs/architecture.md's Phase 4 section explains why that's
  // out of scope for now). 1080p is the default here as of Phase 6 ("push stable 1080p30" exit
  // criterion) -- matches windows/vcam/MediaStream.cpp's now-1080p declared stream size; a
  // mismatch here would silently stretch the image again, the same bug Phase 3C already fixed
  // once for 720p.
  fun startCapture(
    width: Int = 1920,
    height: Int = 1080,
    fps: Int = 30,
    bitrateBps: Int = 6_000_000,
    lensFacing: Int = CameraCharacteristics.LENS_FACING_BACK,
  ) {
    _uiState.value = CaptureUiState.WaitingForConnection
    val context = getApplication<Application>()
    val intent =
      Intent(context, CaptureService::class.java).apply {
        action = CaptureService.ACTION_START
        putExtra(CaptureService.EXTRA_WIDTH, width)
        putExtra(CaptureService.EXTRA_HEIGHT, height)
        putExtra(CaptureService.EXTRA_FPS, fps)
        putExtra(CaptureService.EXTRA_BITRATE, bitrateBps)
        putExtra(CaptureService.EXTRA_LENS_FACING, lensFacing)
      }
    ContextCompat.startForegroundService(context, intent)
  }

  fun stopCapture() {
    val context = getApplication<Application>()
    context.startService(Intent(context, CaptureService::class.java).setAction(CaptureService.ACTION_STOP))
    _uiState.value = CaptureUiState.Idle
  }

  override fun onCleared() {
    super.onCleared()
    statsJob?.cancel()
    // Deliberately not stopping capture here -- CaptureService outliving this ViewModel (e.g. the
    // Activity being destroyed while backgrounded) is the entire point of Phase 5's foreground
    // service. Only unbind; the service keeps running until an explicit ACTION_STOP.
    if (isBound) {
      getApplication<Application>().unbindService(connection)
      isBound = false
    }
  }
}
