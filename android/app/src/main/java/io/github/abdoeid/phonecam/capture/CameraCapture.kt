package io.github.abdoeid.phonecam.capture

import android.content.Context
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
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
 * balance only: manual controls (ISO, exposure time, manual WB) are deferred, so there's no need
 * here to probe MANUAL_SENSOR capability -- see docs/architecture.md.
 */
class CameraCapture(private val context: Context) {
  private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
  private var thread: HandlerThread? = null
  private var handler: Handler? = null
  private var device: CameraDevice? = null
  private var session: CameraCaptureSession? = null

  fun start(
    targetSurface: Surface,
    fps: Int,
    onError: (Throwable) -> Unit,
    lensFacing: Int = CameraCharacteristics.LENS_FACING_BACK,
  ) {
    val bgThread = HandlerThread("CameraCapture").apply { start() }
    thread = bgThread
    val bgHandler = Handler(bgThread.looper)
    handler = bgHandler

    val cameraId = pickCameraId(lensFacing) ?: run {
      onError(IllegalStateException("No camera found for lensFacing=$lensFacing"))
      return
    }

    try {
      @Suppress("MissingPermission") // caller (MainScreen) requests CAMERA before calling start()
      cameraManager.openCamera(
        cameraId,
        object : CameraDevice.StateCallback() {
          override fun onOpened(camera: CameraDevice) {
            device = camera
            openSession(camera, targetSurface, fps, bgHandler, onError)
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
    } catch (e: SecurityException) {
      onError(e)
    }
  }

  private fun openSession(
    camera: CameraDevice,
    targetSurface: Surface,
    fps: Int,
    bgHandler: Handler,
    onError: (Throwable) -> Unit,
  ) {
    val stateCallback =
      object : CameraCaptureSession.StateCallback() {
        override fun onConfigured(configuredSession: CameraCaptureSession) {
          session = configuredSession
          val request =
            camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
              addTarget(targetSurface)
              set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)
              set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(fps, fps))
            }
          configuredSession.setRepeatingRequest(request.build(), null, bgHandler)
        }

        override fun onConfigureFailed(configuredSession: CameraCaptureSession) {
          onError(IllegalStateException("Camera capture session configuration failed"))
        }
      }

    val executor = Executor { command -> bgHandler.post(command) }
    val sessionConfig =
      SessionConfiguration(SessionConfiguration.SESSION_REGULAR, listOf(OutputConfiguration(targetSurface)), executor, stateCallback)
    camera.createCaptureSession(sessionConfig)
  }

  fun stop() {
    session?.close()
    session = null
    device?.close()
    device = null
    thread?.quitSafely()
    thread = null
    handler = null
  }

  private fun pickCameraId(lensFacing: Int): String? =
    cameraManager.cameraIdList.firstOrNull { id ->
      cameraManager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING) == lensFacing
    }
}
