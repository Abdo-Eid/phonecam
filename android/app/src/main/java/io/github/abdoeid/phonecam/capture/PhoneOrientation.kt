package io.github.abdoeid.phonecam.capture

import android.content.Context
import android.view.OrientationEventListener
import kotlin.math.abs

/**
 * Follows how the phone is held, in quarter turns clockwise from upright (0, 90, 180, 270), and
 * calls [onChanged] when that changes. Uses the motion sensor rather than the screen's rotation,
 * which a phone used as a webcam often has locked. Not yet checked with the screen off, when some
 * phones hold back sensor events; the last position is then kept.
 */
class PhoneOrientation(context: Context, private val onChanged: (Int) -> Unit) {
  /** Upright until the sensor says otherwise; also kept while the phone lies flat. */
  @Volatile
  var quarterTurn: Int = 0
    private set

  private val listener =
    object : OrientationEventListener(context) {
      override fun onOrientationChanged(orientation: Int) {
        val next = nextQuarterTurn(quarterTurn, orientation)
        if (next == quarterTurn) return
        quarterTurn = next
        onChanged(next)
      }
    }

  fun start() {
    if (listener.canDetectOrientation()) listener.enable()
  }

  fun stop() = listener.disable()
}

// Beyond 45 degrees the next quarter turn is nearer; waiting a little longer keeps a phone resting
// near the diagonal from flipping the picture back and forth.
private const val SWITCH_DEGREES = 60

/** [raw] is OrientationEventListener's 0..359, or -1 while the phone lies flat. */
internal fun nextQuarterTurn(current: Int, raw: Int): Int {
  if (raw < 0) return current
  val away = abs(((raw - current + 540) % 360) - 180)
  if (away < SWITCH_DEGREES) return current
  return ((raw + 45) / 90 * 90) % 360
}

/**
 * Clockwise degrees the camera's frame must be turned to be upright. The formula is the one
 * Android documents for CaptureRequest.JPEG_ORIENTATION: a front camera faces the other way, so
 * turning the phone turns its picture in the opposite direction.
 */
internal fun uprightRotation(sensorOrientation: Int, frontFacing: Boolean, quarterTurn: Int): Int {
  val turn = if (frontFacing) -quarterTurn else quarterTurn
  return (sensorOrientation + turn + 360) % 360
}
