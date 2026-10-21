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
import io.github.abdoeid.phonecam.control.TrustedPcs
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.receiveAsFlow
import kotlin.math.round

sealed interface CaptureUiState {
  data object Idle : CaptureUiState

  data object WaitingForConnection : CaptureUiState
  data object Stopped : CaptureUiState

  data class Streaming(val elapsedSeconds: Double, val measuredFps: Double) : CaptureUiState

  data class Error(val message: String) : CaptureUiState
}

// Capture itself now lives in CaptureService (see its class doc), not here -- this
// ViewModel only sends start/stop intents and polls the bound service's state. Binding (not
// starting) in init{} so state can be read even if the service was already running from a prior
// Activity instance; ACTION_START/ACTION_STOP intents (not direct method calls on the bound
// service) own the actual start/stop, since that's the pattern startForegroundService() requires
// -- the service must reach startForeground() from within its own onStartCommand.
//
// Poll while the UI is visible, independently of startCapture()/stopCapture().
// Release the binding and polling when hidden; a stopped service then exits,
// while a started streaming service remains alive. CaptureService can also be started by MainActivity's
// AOA accessory-attach handling (ACTION_START_AOA), entirely independent of this ViewModel and
// its Start button, so uiState needs to reflect the service's real state regardless of what
// triggered it. Previously, an AOA-triggered session would stream successfully while this
// ViewModel kept reporting stale Idle indefinitely, since nothing had ever called startCapture()
// to kick off polling.
class MainScreenViewModel(application: Application) : AndroidViewModel(application) {
  private val _uiState = MutableStateFlow<CaptureUiState>(CaptureUiState.Idle)
  val uiState: StateFlow<CaptureUiState> = _uiState.asStateFlow()
  val pairedPcs = MutableStateFlow<List<TrustedPcs.Pc>>(emptyList())
  val pairingMessage = MutableStateFlow("")
  val connectedPc = MutableStateFlow("")
  private val notices = Channel<String>(Channel.BUFFERED)
  val notifications = notices.receiveAsFlow()
  private var announcedStream = false
  private var lastPairStatus = ""

  private var boundService: CaptureService? = null
  private var isBound = false
  private var statsJob: Job? = null
  private var scanningQr = false

  private val connection =
    object : ServiceConnection {
      override fun onServiceConnected(name: ComponentName?, binder: IBinder) {
        boundService = (binder as CaptureService.LocalBinder).getService()
      }

      override fun onServiceDisconnected(name: ComponentName?) {
        boundService = null
      }
    }

  init { bindUi() }

  private fun startPolling() {
    val context = getApplication<Application>()
    statsJob =
      viewModelScope.launch {
        while (true) {
          pairedPcs.value = TrustedPcs(context).list()
          val pairStatus = TrustedPcs.status()
          if (pairStatus.isNotEmpty() && pairStatus != lastPairStatus) {
            pairingMessage.value = pairStatus
            lastPairStatus = pairStatus
            if (pairStatus.startsWith("Paired with")) notices.trySend(pairStatus)
          }
          val service = boundService
          if (service == null) {
            // Bind hasn't completed yet -- init{} kicked it off, but onServiceConnected is async.
            delay(100)
            continue
          }
          val error = service.lastError
          connectedPc.value = service.connectedPc
          if (service.userStopped) {
            if (_uiState.value != CaptureUiState.Stopped) notices.trySend("Stopped")
            _uiState.value = CaptureUiState.Stopped
            announcedStream = false
          } else if (error != null) {
            _uiState.value = CaptureUiState.Error(error.message ?: error.toString())
          } else {
            _uiState.value =
              when (service.state) {
                CaptureState.IDLE -> if (connectedPc.value.isNotEmpty() || TrustedPcs.isPairingPending())
                  CaptureUiState.WaitingForConnection else CaptureUiState.Idle
                CaptureState.WAITING_FOR_CONNECTION -> CaptureUiState.WaitingForConnection
                // Quantized to the precision OsdReadout actually renders -- whole seconds
                // (formatElapsed truncates anyway) and 0.1 fps ("%.1f"). Without this
                // every poll could produce a visibly identical but
                // not-equal Streaming, re-emitting StateFlow and recomposing the whole screen
                // for the same pixels. At this precision consecutive ticks compare equal and
                // StateFlow conflates them.
                CaptureState.STREAMING ->
                  if (!service.hasVideoFrames) CaptureUiState.WaitingForConnection else CaptureUiState.Streaming(
                    elapsedSeconds = service.elapsedSeconds.toLong().toDouble(),
                    measuredFps = round(service.measuredFps * 10.0) / 10.0,
                  )
              }
          }
          if (_uiState.value is CaptureUiState.Streaming && !announcedStream) {
            announcedStream = true
            notices.trySend("Connected · streaming")
          }
          delay(150)
        }
      }
  }

  // TODO: resolution/fps become PC-driven SetResolution/SetFps commands once the vcam
  // supports a non-fixed stream size (docs/architecture.md's control-channel section explains why that's
  // out of scope for now). 1080p is the default here (the "push stable 1080p30" exit
  // criterion) -- matches windows/vcam/MediaStream.cpp's now-1080p declared stream size; a
  // mismatch here would silently stretch the image again, the same bug already fixed
  // once for 720p.
  fun startCapture(
    width: Int = 1920,
    height: Int = 1080,
    fps: Int = 30,
    bitrateBps: Int = 6_000_000,
    lensFacing: Int = CameraCharacteristics.LENS_FACING_BACK,
  ) {
    if (scanningQr) return
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
    _uiState.value = CaptureUiState.Stopped
    announcedStream = false
    notices.trySend("Stopped")
  }

  fun beginQrScan(): Boolean {
    val service = boundService ?: run { pairingMessage.value = "PhoneCam is starting. Try Scan PC QR again."; return false }
    scanningQr = true
    TrustedPcs.cancelPending()
    pairingMessage.value = ""
    service.beginQrScan()
    return true
  }
  fun qrScanned(payload: String?) {
    try {
      if (payload != null) TrustedPcs(getApplication()).scan(payload)
    } catch (_: IllegalArgumentException) {
      pairingMessage.value = "This is not a valid PhoneCam PC QR code."
    } finally {
      scanningQr = false
      boundService?.endQrScan()
    }
  }
  fun forgetPc(id: String) { boundService?.forgetPc(id) }

  fun bindUi() {
    if (isBound) return
    val context = getApplication<Application>()
    isBound = context.bindService(Intent(context, CaptureService::class.java), connection, Context.BIND_AUTO_CREATE)
    if (isBound) startPolling()
  }

  fun unbindUi() {
    // The scanner is another PhoneCam activity, not an app exit.
    if (scanningQr) return
    releaseBinding()
  }

  private fun releaseBinding() {
    if (!isBound) return
    statsJob?.cancel()
    statsJob = null
    getApplication<Application>().unbindService(connection)
    isBound = false
    boundService = null
    connectedPc.value = ""
  }

  override fun onCleared() {
    super.onCleared()
    statsJob?.cancel()
    // An active started service survives; a stopped service exits with the UI.
    releaseBinding()
  }
}
