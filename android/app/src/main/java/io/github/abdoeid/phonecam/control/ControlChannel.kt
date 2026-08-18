package io.github.abdoeid.phonecam.control

import android.content.Context
import com.google.flatbuffers.FlatBufferBuilder
import io.github.abdoeid.phonecam.transport.ControlSocketServer
import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean
import phonecam.control.Ack
import phonecam.control.AckResult
import phonecam.control.ControlEnvelope
import phonecam.control.ControlPayload
import phonecam.control.CurrentSettings
import phonecam.control.LensChanged
import phonecam.control.RequestKeyframe
import phonecam.control.SetBitrate
import phonecam.control.SetEv
import phonecam.control.SetFocusMode
import phonecam.control.SetLens
import phonecam.control.SetTorch
import phonecam.control.SetZoomRatio
import phonecam.control.TapToFocus

/**
 * Handlers for PC -> phone commands this phase implements. Handlers run on ControlChannel's
 * receive thread; implementations that touch Camera2/MediaCodec objects owned by another thread
 * hop appropriately themselves (same pattern CaptureController already uses for camera callbacks).
 * Each handler returns an [AckResult] so ControlChannel can send the Ack -- see
 * docs/control-protocol.md's cmd_id/Ack correlation discipline.
 */
interface ControlCommandListener {
  fun onSetZoomRatio(ratio: Float): Byte
  fun onSetEv(steps: Int): Byte
  fun onSetTorch(on: Boolean): Byte
  fun onSetFocusMode(mode: Byte): Byte
  fun onTapToFocus(nx: Float, ny: Float, regionSize: Float): Byte
  fun onRequestKeyframe(): Byte
  fun onSetBitrate(bitrateBps: Int): Byte
  fun onSetLens(cameraId: String): Byte
}

/**
 * Owns the phonecam_control socket for one capture session: accepts the PC's connection, sends
 * CapabilityDescriptor immediately (docs/wire-protocol.md's control-channel handshake), then
 * dispatches incoming commands to [listener] and exposes send* methods for telemetry.
 *
 * Scoped to this phase: control and video share one start()/stop() lifecycle via
 * CaptureController (unlike the full protocol doc, which allows control to outlive a Stop) --
 * making control durable across video Stop/Start is Phase 5 robustness scope.
 */
class ControlChannel(private val context: Context, private val listener: ControlCommandListener) {
  private val server = ControlSocketServer()
  private var output: OutputStream? = null
  private var receiveThread: Thread? = null
  private val stopped = AtomicBoolean(false)

  /**
   * Binds and blocks in accept() until the PC connects (or [stop] unblocks it), then sends
   * CapabilityDescriptor and starts the receive loop on a new background thread. Call this from a
   * background thread -- accept() blocks.
   */
  fun start() {
    stopped.set(false)
    server.open()
    val (input, out) = server.accept()
    if (stopped.get()) {
      try {
        input.close()
        out.close()
      } catch (_: IOException) {
        // already torn down
      }
      return
    }
    output = out
    sendCapabilities(out)
    receiveThread = Thread({ receiveLoop(input) }, "ControlChannel-recv").apply { start() }
  }

  fun stop() {
    if (!stopped.compareAndSet(false, true)) return
    server.close() // unblocks a pending accept() or receiveLoop's read
    if (receiveThread !== Thread.currentThread()) {
      receiveThread?.join(1000)
    }
    receiveThread = null
    output = null
  }

  fun sendAck(cmdId: Int, result: Byte, message: String = "") {
    val builder = FlatBufferBuilder(128)
    val messageOffset = if (message.isNotEmpty()) builder.createString(message) else 0
    val ackOffset = Ack.createAck(builder, result, messageOffset)
    val envelope = ControlEnvelope.createControlEnvelope(builder, cmdId.toUInt(), ControlPayload.Ack, ackOffset)
    ControlEnvelope.finishControlEnvelopeBuffer(builder, envelope)
    sendBytes(builder)
  }

  fun sendLensChanged(cameraId: String) {
    val builder = FlatBufferBuilder(64)
    val idOffset = builder.createString(cameraId)
    val payloadOffset = LensChanged.createLensChanged(builder, idOffset)
    val envelope = ControlEnvelope.createControlEnvelope(builder, 0u, ControlPayload.LensChanged, payloadOffset)
    ControlEnvelope.finishControlEnvelopeBuffer(builder, envelope)
    sendBytes(builder)
  }

  fun sendCurrentSettings(
    cameraId: String,
    width: Int,
    height: Int,
    fps: Int,
    bitrateBps: Int,
    zoomRatio: Float,
    focusMode: Byte,
    evSteps: Int,
    torchOn: Boolean,
  ) {
    val builder = FlatBufferBuilder(128)
    val idOffset = builder.createString(cameraId)
    val payloadOffset =
      CurrentSettings.createCurrentSettings(
        builder, idOffset, width.toUShort(), height.toUShort(), fps.toUShort(), bitrateBps.toUInt(), zoomRatio,
        focusMode, evSteps, false, 0, 0L, 0, 0u, torchOn, false,
      )
    val envelope = ControlEnvelope.createControlEnvelope(builder, 0u, ControlPayload.CurrentSettings, payloadOffset)
    ControlEnvelope.finishControlEnvelopeBuffer(builder, envelope)
    sendBytes(builder)
  }

  private fun sendCapabilities(out: OutputStream) {
    val builder = FlatBufferBuilder(1024)
    val capsOffset = CameraCapabilityProbe.buildCapabilityDescriptor(context, builder)
    val envelope =
      ControlEnvelope.createControlEnvelope(
        builder, 0u, ControlPayload.phonecam_wire_CapabilityDescriptor, capsOffset,
      )
    ControlEnvelope.finishControlEnvelopeBuffer(builder, envelope)
    try {
      val buf = builder.dataBuffer()
      val bytes = ByteArray(buf.remaining())
      buf.get(bytes)
      ControlFraming.writeEnvelope(out, bytes)
    } catch (_: IOException) {
      // handled by the receive loop's own EOF/IO handling once it starts
    }
  }

  private fun sendBytes(builder: FlatBufferBuilder) {
    val out = output ?: return
    try {
      val buf = builder.dataBuffer()
      val bytes = ByteArray(buf.remaining())
      buf.get(bytes)
      ControlFraming.writeEnvelope(out, bytes)
    } catch (_: IOException) {
      // Best-effort: a send failure means the connection is already broken;
      // the receive loop's own EOF/IO handling is what tears the session down.
    }
  }

  private fun receiveLoop(input: InputStream) {
    try {
      while (!stopped.get()) {
        dispatch(ControlFraming.readEnvelope(input))
      }
    } catch (_: EOFException) {
      // normal: PC disconnected or stop() closed the socket
    } catch (_: IOException) {
      // normal: stop() closed the socket out from under a blocked read
    }
  }

  private fun dispatch(bytes: ByteArray) {
    val envelope = ControlEnvelope.getRootAsControlEnvelope(ByteBuffer.wrap(bytes))
    val cmdId = envelope.cmdId.toInt()
    // A command handler throwing (e.g. a Camera2 API call failing synchronously) must not crash
    // this thread -- confirmed live: an uncaught CameraAccessException from a SetLens handler
    // took down the whole app, since this thread has no other exception handler. Report
    // Ack{Error} instead and keep the receive loop alive for the next command.
    val result: Byte =
      try {
        dispatchToListener(envelope)
      } catch (e: Exception) {
        sendAck(cmdId, AckResult.Error, e.message ?: e.toString())
        return
      }
    sendAck(cmdId, result)
  }

  private fun dispatchToListener(envelope: ControlEnvelope): Byte =
    when (envelope.payloadType) {
        ControlPayload.SetZoomRatio -> {
          val t = SetZoomRatio()
          envelope.payload(t)
          listener.onSetZoomRatio(t.ratio)
        }
        ControlPayload.SetEv -> {
          val t = SetEv()
          envelope.payload(t)
          listener.onSetEv(t.steps)
        }
        ControlPayload.SetTorch -> {
          val t = SetTorch()
          envelope.payload(t)
          listener.onSetTorch(t.on)
        }
        ControlPayload.SetFocusMode -> {
          val t = SetFocusMode()
          envelope.payload(t)
          listener.onSetFocusMode(t.mode)
        }
        ControlPayload.TapToFocus -> {
          val t = TapToFocus()
          envelope.payload(t)
          listener.onTapToFocus(t.nx, t.ny, t.regionSize)
        }
        ControlPayload.RequestKeyframe -> listener.onRequestKeyframe()
        ControlPayload.SetBitrate -> {
          val t = SetBitrate()
          envelope.payload(t)
          listener.onSetBitrate(t.bitrateBps.toInt())
        }
        ControlPayload.SetLens -> {
          val t = SetLens()
          envelope.payload(t)
          listener.onSetLens(t.cameraId ?: "")
        }
      else -> AckResult.Unsupported
    }
}
