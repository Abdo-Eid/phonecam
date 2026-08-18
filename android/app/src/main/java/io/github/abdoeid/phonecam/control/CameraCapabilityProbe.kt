package io.github.abdoeid.phonecam.control

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.os.Build
import com.google.flatbuffers.FlatBufferBuilder
import phonecam.wire.AfMode
import phonecam.wire.AwbMode
import phonecam.wire.CapabilityDescriptor
import phonecam.wire.HardwareLevel
import phonecam.wire.LensCapabilities
import phonecam.wire.LensFacing
import phonecam.wire.Range32
import phonecam.wire.RangeF

/**
 * Queries Camera2's real capabilities for every lens on this device and encodes them as a
 * phonecam.wire.CapabilityDescriptor FlatBuffer (proto/wire.fbs), sent once per control-channel
 * connect (docs/control-protocol.md). This is the source of truth the PC UI builds itself from --
 * it must never claim a capability this probe didn't actually find (see docs/architecture.md risk
 * R9 re: this LIMITED-hardware-level device).
 *
 * `resolutions` is left empty deliberately: SetResolution and dynamic vcam stream sizing are
 * explicitly out of Phase 4 scope (windows/vcam/MediaStream.cpp has a single fixed stream size --
 * see docs/architecture.md's Phase 4 section), so there's nothing on the PC side yet that would
 * consume a resolution list.
 */
object CameraCapabilityProbe {
  /** Returns an unfinished offset for a CapabilityDescriptor table -- caller wraps it in a ControlEnvelope. */
  fun buildCapabilityDescriptor(context: Context, builder: FlatBufferBuilder): Int {
    val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    val lensOffsets =
      cameraManager.cameraIdList.map { id -> buildLensCapabilities(builder, cameraManager, id) }.toIntArray()
    val lensesOffset = CapabilityDescriptor.createLensesVector(builder, lensOffsets)
    val deviceModelOffset = builder.createString(Build.MODEL ?: "unknown")
    val androidReleaseOffset = builder.createString(Build.VERSION.RELEASE ?: "unknown")

    CapabilityDescriptor.startCapabilityDescriptor(builder)
    CapabilityDescriptor.addLenses(builder, lensesOffset)
    CapabilityDescriptor.addAndroidRelease(builder, androidReleaseOffset)
    CapabilityDescriptor.addDeviceModel(builder, deviceModelOffset)
    CapabilityDescriptor.addProtocolVersion(builder, 1u)
    return CapabilityDescriptor.endCapabilityDescriptor(builder)
  }

  private fun buildLensCapabilities(builder: FlatBufferBuilder, cameraManager: CameraManager, cameraId: String): Int {
    val chars = cameraManager.getCameraCharacteristics(cameraId)

    val capabilities = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: IntArray(0)
    val hasManualSensor = capabilities.contains(CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)

    // CONTROL_ZOOM_RATIO_RANGE requires API 30 (minSdk is 28, see build.gradle.kts) -- null below
    // 30 means addZoomRatioRange reports a fixed 1.0..1.0 range, so the PC's capability-driven UI
    // just never shows a zoom control on those devices, same as any other unsupported capability.
    val zoomRange = if (Build.VERSION.SDK_INT >= 30) chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) else null
    val evRange = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
    val evStep = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)
    val isoRange = if (hasManualSensor) chars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE) else null
    val expRange = if (hasManualSensor) chars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) else null
    val afModes =
      (chars.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: IntArray(0)).toList().mapNotNull(::toAfMode)
    val awbModes =
      (chars.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES) ?: IntArray(0)).toList().mapNotNull(::toAwbMode)
    val hasTorch = chars.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) ?: false
    val hasOis =
      !(chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION) ?: IntArray(0)).let {
        it.isEmpty() || (it.size == 1 && it[0] == CameraCharacteristics.LENS_OPTICAL_STABILIZATION_MODE_OFF)
      }

    val cameraIdOffset = builder.createString(cameraId)
    val afModesOffset = LensCapabilities.createAfModesVector(builder, afModes.toByteArray())
    val awbModesOffset = LensCapabilities.createAwbModesVector(builder, awbModes.toByteArray())

    LensCapabilities.startLensCapabilities(builder)
    LensCapabilities.addCameraId(builder, cameraIdOffset)
    LensCapabilities.addFacing(builder, toLensFacing(chars.get(CameraCharacteristics.LENS_FACING)))
    LensCapabilities.addHardwareLevel(builder, toHardwareLevel(chars.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)))
    LensCapabilities.addHasManualSensor(builder, hasManualSensor)
    LensCapabilities.addZoomRatioRange(
      builder,
      RangeF.createRangeF(builder, zoomRange?.lower ?: 1.0f, zoomRange?.upper ?: 1.0f, 0.0f),
    )
    LensCapabilities.addIsoRange(
      builder,
      Range32.createRange32(builder, isoRange?.lower ?: 0, isoRange?.upper ?: 0, 0),
    )
    LensCapabilities.addExposureTimeNsRange(
      builder,
      Range32.createRange32(
        builder,
        (expRange?.lower ?: 0L).coerceIn(Int.MIN_VALUE.toLong(), Int.MAX_VALUE.toLong()).toInt(),
        (expRange?.upper ?: 0L).coerceIn(Int.MIN_VALUE.toLong(), Int.MAX_VALUE.toLong()).toInt(),
        0,
      ),
    )
    LensCapabilities.addEvCompensationRange(
      builder,
      Range32.createRange32(builder, evRange?.lower ?: 0, evRange?.upper ?: 0, 0),
    )
    LensCapabilities.addEvStep(builder, evStep?.toFloat() ?: 0.0f)
    LensCapabilities.addAfModes(builder, afModesOffset)
    LensCapabilities.addAwbModes(builder, awbModesOffset)
    LensCapabilities.addHasTorch(builder, hasTorch)
    LensCapabilities.addHasOpticalStabilization(builder, hasOis)
    return LensCapabilities.endLensCapabilities(builder)
  }

  private fun toLensFacing(facing: Int?): Byte =
    when (facing) {
      CameraCharacteristics.LENS_FACING_FRONT -> LensFacing.Front
      CameraCharacteristics.LENS_FACING_EXTERNAL -> LensFacing.External
      else -> LensFacing.Back
    }

  private fun toHardwareLevel(level: Int?): Byte =
    when (level) {
      CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED -> HardwareLevel.Limited
      CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_FULL -> HardwareLevel.Full
      CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_3 -> HardwareLevel.Level3
      CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL -> HardwareLevel.External
      else -> HardwareLevel.Legacy
    }

  // Camera2's CONTINUOUS_VIDEO and CONTINUOUS_PICTURE both collapse onto our single Continuous
  // value -- the wire protocol doesn't need the video/picture distinction since this app only
  // ever streams video.
  private fun toAfMode(mode: Int): Byte? =
    when (mode) {
      CameraMetadata.CONTROL_AF_MODE_OFF -> AfMode.Off
      CameraMetadata.CONTROL_AF_MODE_AUTO -> AfMode.Auto
      CameraMetadata.CONTROL_AF_MODE_MACRO -> AfMode.Macro
      CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE -> AfMode.Continuous
      CameraMetadata.CONTROL_AF_MODE_EDOF -> AfMode.EdgeCapture
      else -> null
    }

  // WARM_FLUORESCENT has no counterpart in phonecam.wire.AwbMode -- dropped, not mapped.
  private fun toAwbMode(mode: Int): Byte? =
    when (mode) {
      CameraMetadata.CONTROL_AWB_MODE_OFF -> AwbMode.Off
      CameraMetadata.CONTROL_AWB_MODE_AUTO -> AwbMode.Auto
      CameraMetadata.CONTROL_AWB_MODE_INCANDESCENT -> AwbMode.Incandescent
      CameraMetadata.CONTROL_AWB_MODE_FLUORESCENT -> AwbMode.Fluorescent
      CameraMetadata.CONTROL_AWB_MODE_DAYLIGHT -> AwbMode.Daylight
      CameraMetadata.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT -> AwbMode.CloudyDaylight
      CameraMetadata.CONTROL_AWB_MODE_TWILIGHT -> AwbMode.Twilight
      CameraMetadata.CONTROL_AWB_MODE_SHADE -> AwbMode.Shade
      else -> null
    }

  private fun List<Byte>.toByteArray(): ByteArray = ByteArray(size) { this[it] }
}
