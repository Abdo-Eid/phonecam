package io.github.abdoeid.phonecam

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.view.Surface
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import io.github.abdoeid.phonecam.bench.BenchEncoderSettings
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Real codec configure/start probes, no camera, app settings or transport changes. */
@RunWith(AndroidJUnit4::class)
class EncoderInventoryTest {
  private fun baseFormat() = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, 1920, 1080).apply {
    setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
    setInteger(MediaFormat.KEY_BIT_RATE, 6_000_000)
    setInteger(MediaFormat.KEY_FRAME_RATE, 30)
    setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
    setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
  }

  @Test fun inventoryAndProbeIndividualKeys() {
    val report = JSONObject().put("device", Build.MODEL).put("sdk", Build.VERSION.SDK_INT)
    val inventory = JSONArray()
    for (info in MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos) {
      if (!info.isEncoder || info.supportedTypes.none { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) }) continue
      val caps = info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
      val video = requireNotNull(caps.videoCapabilities)
      val encoder = requireNotNull(caps.encoderCapabilities)
      inventory.put(JSONObject().apply {
        put("name", info.name)
        if (Build.VERSION.SDK_INT >= 29) put("hardware_accelerated", info.isHardwareAccelerated)
        put("profiles_levels", JSONArray(caps.profileLevels.map {
          JSONObject().put("profile", it.profile).put("level", it.level)
        }))
        put("color_formats", JSONArray(caps.colorFormats.toList()))
        put("widths", video.supportedWidths.toString())
        put("heights", video.supportedHeights.toString())
        put("frame_rates", video.supportedFrameRates.toString())
        put("bitrate", video.bitrateRange.toString())
        put("supports_1080p30", video.areSizeAndRateSupported(1920, 1080, 30.0))
        put("supports_720p30", video.areSizeAndRateSupported(1280, 720, 30.0))
        put("cbr", encoder.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
        put("intra_refresh_feature", caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_IntraRefresh))
      })
    }
    report.put("encoders", inventory)
    val probes = JSONArray()
    var defaultPassed = false
    for (name in listOf("default") + BenchEncoderSettings.tags.keys) {
      val result = JSONObject().put("case", name)
      var codec: MediaCodec? = null
      var surface: Surface? = null
      var started = false
      var stage = "create"
      try {
        codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        result.put("codec", codec.name)
        val format = baseFormat()
        stage = "apply-key"
        BenchEncoderSettings.applyCase(name, format)
        result.put("requested_format", format.toString())
        stage = "configure"
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        stage = "surface"
        surface = codec.createInputSurface()
        stage = "start"
        codec.start()
        started = true
        result.put("accepted", true)
        if (name == "default") defaultPassed = true
      } catch (error: Exception) {
        result.put("accepted", false).put("failed_stage", stage)
          .put("exception", error.javaClass.name).put("message", error.message)
        if (error is MediaCodec.CodecException) {
          result.put("error_code", error.errorCode).put("diagnostic", error.diagnosticInfo)
            .put("recoverable", error.isRecoverable).put("transient", error.isTransient)
        }
      } finally {
        if (started) runCatching { codec?.stop() }.onFailure { result.put("stop_error", it.toString()) }
        runCatching { codec?.release() }.onFailure { result.put("release_error", it.toString()) }
        surface?.release()
      }
      probes.put(result)
    }
    report.put("probes", probes)
    report.put("caveat", "configure/start acceptance only; no input frames, no proof that optional keys are honored or latency is lower")
    val context = InstrumentationRegistry.getInstrumentation().targetContext
    val output = context.filesDir.resolve("bench").apply { mkdirs() }.resolve("encoder-inventory.json")
    output.writeText(report.toString(2))
    println("Encoder inventory written to ${output.absolutePath}")
    assertTrue("The unchanged default codec must configure/start", defaultPassed)
  }
}
