package io.github.abdoeid.phonecam.bench

import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.util.Log

/** Debug-only one-variable experiments. No tag selected means the product default. */
object BenchEncoderSettings {
  val tags = linkedMapOf(
    "low_latency" to "PCBenchLowLatency",
    "latency0" to "PCBenchLatency0",
    "priority0" to "PCBenchPriority0",
    "max_b_frames0" to "PCBenchNoBFrames",
    "intra_refresh30" to "PCBenchIntraRefresh",
    "idr2" to "PCBenchIdr2",
    "idr5" to "PCBenchIdr5",
    "profile_baseline" to "PCBenchBaseline",
    "profile_constrained_baseline" to "PCBenchConstrained",
  )

  fun applySelected(format: MediaFormat) {
    if (!BenchTelemetry.enabled) return
    val selected = tags.filterValues { Log.isLoggable(it, Log.DEBUG) }.keys
    check(selected.size <= 1) { "Select only one PhoneCam encoder experiment: $selected" }
    val name = selected.singleOrNull() ?: "default"
    applyCase(name, format)
    Log.d("PhoneCamBench", "encoder_case name=$name")
  }

  // Also called by the debug instrumentation inventory, not by normal startup.
  fun applyCase(name: String, format: MediaFormat) {
    when (name) {
      "default" -> Unit
      "low_latency" -> {
        check(Build.VERSION.SDK_INT >= 30) { "low-latency key needs API 30" }
        format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
      }
      "latency0" -> format.setInteger(MediaFormat.KEY_LATENCY, 0)
      "priority0" -> format.setInteger(MediaFormat.KEY_PRIORITY, 0)
      "max_b_frames0" -> {
        check(Build.VERSION.SDK_INT >= 29) { "max-bframes key needs API 29" }
        format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
      }
      "intra_refresh30" -> format.setInteger(MediaFormat.KEY_INTRA_REFRESH_PERIOD, 30)
      "idr2" -> format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
      "idr5" -> format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 5)
      "profile_baseline" -> format.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
      "profile_constrained_baseline" -> format.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedBaseline)
      else -> error("Unknown encoder experiment: $name")
    }
  }
}
