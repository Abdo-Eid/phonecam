package io.github.abdoeid.phonecam.capture

import android.content.Context
import android.graphics.Rect
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.os.Handler
import android.os.HandlerThread
import android.util.Range
import android.view.Surface
import java.util.concurrent.Executor

/**
 * Opens a camera and streams into [targetSurface] (the H264Encoder's input Surface --
 * zero-copy, Camera2 renders straight into what the encoder reads). Auto exposure/focus/white
 * balance only: manual controls (ISO, exposure time, manual WB) are deferred -- see
 * docs/architecture.md.
 *
 * Phase 4 adds live control-channel updates (zoom/EV/torch/focus/tap-to-focus, all via
 * re-submitting an updated repeating request on the already-open session) and [switchLens], which
 * tears down and reopens just the camera device/session -- targetSurface (and therefore the
 * encoder and video socket) stays untouched, so a lens switch doesn't interrupt streaming beyond
 * a brief reconfiguration gap.
 */
class CameraCapture(private val context: Context) {
  private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
  private var thread: HandlerThread? = null
  private var handler: Handler? = null
  private var device: CameraDevice? = null
  private var session: CameraCaptureSession? = null
  private var requestBuilder: CaptureRequest.Builder? = null
  private var targetSurface: Surface? = null
  private var fps: Int = 30

  var currentCameraId: String? = null
    private set

  /** Fired (on the CameraCapture HandlerThread) whenever a camera device finishes opening -- the
   * first open included, so a caller that only wants lens-switch confirmations should compare
   * against the id it already knows about. */
  var onCameraReady: ((cameraId: String) -> Unit)? = null

  fun start(
    targetSurface: Surface,
    fps: Int,
    onError: (Throwable) -> Unit,
    lensFacing: Int = CameraCharacteristics.LENS_FACING_BACK,
  ) {
    this.targetSurface = targetSurface
    this.fps = fps
    val bgThread = HandlerThread("CameraCapture").apply { start() }
    thread = bgThread
    val bgHandler = Handler(bgThread.looper)
    handler = bgHandler
    openCamera(lensFacing, bgHandler, onError)
  }

  /**
   * Switches to a different lens (by CapabilityDescriptor.lenses[].camera_id, per SetLens in
   * proto/control.fbs) without touching the encoder/socket: closes the current device/session and
   * opens a new one on the same background thread and the same [targetSurface]. Call from any
   * thread (mirrors Camera2's own close()/openCamera() thread-safety); the actual reconfiguration
   * completes asynchronously on this instance's HandlerThread, same as [start].
   */
  fun switchLens(cameraId: String, onError: (Throwable) -> Unit) {
    val bgHandler = handler ?: run {
      onError(IllegalStateException("switchLens called before start()"))
      return
    }
    session?.close()
    session = null
    device?.close()
    device = null
    requestBuilder = null
    openCameraById(cameraId, bgHandler, onError)
  }

  private fun openCamera(lensFacing: Int, bgHandler: Handler, onError: (Throwable) -> Unit) {
    val cameraId = pickCameraId(lensFacing) ?: run {
      onError(IllegalStateException("No camera found for lensFacing=$lensFacing"))
      return
    }
    openCameraById(cameraId, bgHandler, onError)
  }

  private fun openCameraById(cameraId: String, bgHandler: Handler, onError: (Throwable) -> Unit) {
    try {
      @Suppress("MissingPermission") // caller (MainScreen) requests CAMERA before calling start()
      cameraManager.openCamera(
        cameraId,
        object : CameraDevice.StateCallback() {
          override fun onOpened(camera: CameraDevice) {
            device = camera
            currentCameraId = cameraId
            onCameraReady?.invoke(cameraId)
            openSession(camera, bgHandler, onError)
          }

          override fun onDisconnected(camera: CameraDevice) {
            camera.close()
          }

          override fun onError(camera: CameraDevice, error: Int) {
            camera.close()
            onError(IllegalStateException("Camera error code $error"))
          }
        },
        bgHandler,
      )
    } catch (e: CameraAccessException) {
      // openCamera() also throws this synchronously (e.g. "disabled by policy" -- confirmed live
      // on this device via a SetLens command that crashed the whole app before this catch
      // existed: uncaught on the control channel's receive thread takes the whole process down,
      // not just this capture session). SecurityException alone doesn't cover it.
      onError(e)
    } catch (e: SecurityException) {
      onError(e)
    }
  }

  private fun openSession(camera: CameraDevice, bgHandler: Handler, onError: (Throwable) -> Unit) {
    val surface = targetSurface ?: return
    val stateCallback =
      object : CameraCaptureSession.StateCallback() {
        override fun onConfigured(configuredSession: CameraCaptureSession) {
          session = configuredSession
          val builder =
            camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
              addTarget(surface)
              set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)
              set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(fps, fps))
            }
          requestBuilder = builder
          configuredSession.setRepeatingRequest(builder.build(), null, bgHandler)
        }

        override fun onConfigureFailed(configuredSession: CameraCaptureSession) {
          onError(IllegalStateException("Camera capture session configuration failed"))
        }
      }

    val executor = Executor { command -> bgHandler.post(command) }
    val sessionConfig =
      SessionConfiguration(SessionConfiguration.SESSION_REGULAR, listOf(OutputConfiguration(surface)), executor, stateCallback)
    camera.createCaptureSession(sessionConfig)
  }

  fun stop() {
    session?.close()
    session = null
    device?.close()
    device = null
    requestBuilder = null
    thread?.quitSafely()
    thread = null
    handler = null
  }

  // --- live control-channel updates (docs/control-protocol.md) ---
  // Each returns whether the update was actually submitted (false => no open session yet, e.g.
  // a command arrived mid-lens-switch) so ControlChannel can Ack{Busy} instead of Ack{Ok}.

  fun setZoomRatio(ratio: Float): Boolean = updateRequest { it.set(CaptureRequest.CONTROL_ZOOM_RATIO, ratio) }

  fun setEv(steps: Int): Boolean = updateRequest { it.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, steps) }

  fun setTorch(on: Boolean): Boolean =
    updateRequest {
      it.set(CaptureRequest.FLASH_MODE, if (on) CameraMetadata.FLASH_MODE_TORCH else CameraMetadata.FLASH_MODE_OFF)
    }

  fun setFocusMode(camera2AfMode: Int): Boolean = updateRequest { it.set(CaptureRequest.CONTROL_AF_MODE, camera2AfMode) }

  // AF_REGIONS/AE_REGIONS are persisted onto the repeating request like the other live
  // controls, but AF_TRIGGER is NOT: requestBuilder is reused for every future
  // setRepeatingRequest call (zoom, EV, ...), so leaving TRIGGER_START set on it would
  // re-submit an AF trigger on every unrelated command from then on. The actual trigger is a
  // one-shot capture() instead, with the builder's trigger field reset to IDLE right after --
  // this is the standard Camera2 tap-to-focus pattern.
  fun tapToFocus(nx: Float, ny: Float, regionSize: Float): Boolean {
    val rect = sensorRectFromNormalized(nx, ny, regionSize) ?: return false
    val activeSession = session ?: return false
    val builder = requestBuilder ?: return false
    val bgHandler = handler ?: return false
    val region = arrayOf(MeteringRectangle(rect, MeteringRectangle.METERING_WEIGHT_MAX - 1))
    builder.set(CaptureRequest.CONTROL_AF_REGIONS, region)
    builder.set(CaptureRequest.CONTROL_AE_REGIONS, region)
    return try {
      activeSession.setRepeatingRequest(builder.build(), null, bgHandler)
      builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_START)
      activeSession.capture(builder.build(), null, bgHandler)
      builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
      true
    } catch (_: Exception) {
      false
    }
  }

  private fun updateRequest(mutate: (CaptureRequest.Builder) -> Unit): Boolean {
    val activeSession = session ?: return false
    val builder = requestBuilder ?: return false
    val bgHandler = handler ?: return false
    mutate(builder)
    return try {
      activeSession.setRepeatingRequest(builder.build(), null, bgHandler)
      true
    } catch (_: Exception) {
      false
    }
  }

  // nx/ny in [0,1] (0,0 = top-left of the active frame); regionSize is normalized to the
  // shorter dimension -- see proto/control.fbs's TapToFocus doc comment.
  private fun sensorRectFromNormalized(nx: Float, ny: Float, regionSize: Float): Rect? {
    val id = currentCameraId ?: return null
    val activeArray =
      cameraManager.getCameraCharacteristics(id).get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return null
    val shorter = minOf(activeArray.width(), activeArray.height())
    val half = (regionSize * shorter / 2f).toInt().coerceAtLeast(1)
    val cx = (nx * activeArray.width()).toInt()
    val cy = (ny * activeArray.height()).toInt()
    val left = (cx - half).coerceIn(0, activeArray.width() - 1)
    val top = (cy - half).coerceIn(0, activeArray.height() - 1)
    val right = (cx + half).coerceIn(left + 1, activeArray.width())
    val bottom = (cy + half).coerceIn(top + 1, activeArray.height())
    return Rect(left, top, right, bottom)
  }

  private fun pickCameraId(lensFacing: Int): String? =
    cameraManager.cameraIdList.firstOrNull { id ->
      cameraManager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING) == lensFacing
    }
}
