package io.github.abdoeid.phonecam.bench

import android.util.Log
import io.github.abdoeid.phonecam.BuildConfig

/**
 * Development-only per-frame timing lines for the media-pipeline measurements.
 *
 * Disabled by default. Enable on a connected device before launching the app:
 *
 *   adb shell setprop log.tag.PhoneCamBench DEBUG
 *
 * and read them with:
 *
 *   adb logcat -s PhoneCamBench:D
 *
 * These lines intentionally carry the original camera/encoder timestamps so they can be joined
 * with the host's `BENCH` lines by presentation timestamp. Keep the tag and field names stable;
 * the offline measurement script parses them.
 */
object BenchTelemetry {
  private const val TAG = "PhoneCamBench"

  val enabled: Boolean = BuildConfig.DEBUG && Log.isLoggable(TAG, Log.DEBUG)

  fun cameraStart(cameraId: String, fps: Int) {
    if (enabled) Log.d(TAG, "camera_start camera_id=$cameraId requested_fps=$fps")
  }

  fun sensor(sensorTimestampNs: Long, frameDurationNs: Long, aeFpsRange: String) {
    if (enabled) {
      Log.d(
        TAG,
        "sensor ts_ns=$sensorTimestampNs frame_ns=$frameDurationNs ae_fps=$aeFpsRange",
      )
    }
  }

  fun encoderFormat(codecName: String, outputFormat: String) {
    if (enabled) Log.d(TAG, "encoder_format codec=$codecName format=$outputFormat")
  }

  fun encoderFrame(
    ptsUs: Long,
    outputNs: Long,
    outputUptimeMs: Long,
    dequeueWaitUs: Long,
    writeUs: Long,
    size: Int,
    keyframe: Boolean,
    sequence: Int,
  ) {
    if (enabled) {
      Log.d(
        TAG,
        "encoder_frame seq=$sequence pts_us=$ptsUs output_ns=$outputNs " +
          "uptime_ms=$outputUptimeMs dequeue_wait_us=$dequeueWaitUs write_us=$writeUs " +
          "size=$size key=${if (keyframe) 1 else 0}",
      )
    }
  }
}
