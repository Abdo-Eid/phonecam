package io.github.abdoeid.phonecam.control

import android.content.Context
import android.util.Log
import com.google.flatbuffers.FlatBufferBuilder
import io.github.abdoeid.phonecam.transport.ControlSocketServer
import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.SocketTimeoutException
import java.net.Socket
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean
import phonecam.control.Ack
import phonecam.control.AckResult
import phonecam.control.AuthChallenge
import phonecam.control.AuthResponse
import phonecam.control.ControlEnvelope
import phonecam.control.ControlPayload
import phonecam.control.CurrentSettings
import phonecam.control.Error
import phonecam.control.LensChanged
import phonecam.control.RequestKeyframe
import phonecam.control.SetBitrate
import phonecam.control.SetEv
import phonecam.control.SetFocusMode
import phonecam.control.SetLens
import phonecam.control.SetTorch
import phonecam.control.SetZoomRatio
import phonecam.control.StartVideo
import phonecam.control.Stats
import phonecam.control.StopVideo
import phonecam.control.TapToFocus

/**
 * Handlers for PC -> phone commands this channel implements. Handlers run on ControlChannel's
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

  /**
   * Start streaming live camera video to the PC. [mode] says how (framed TCP
   * socket, or RTP over UDP); [rtpPort] is the PC-side UDP port for RTP mode.
   *
   * The PC's address comes from the control connection itself (the phone
   * learns it from the socket the PC connected on) -- there is no address to
   * type on the phone and no discovery protocol to speak of.
   *
   * Return AckResult.Ok only if the session actually started; the reply is what the PC
   * treats as "video is coming".
   *
   * The default is "not implemented here": the camera-command listener in CaptureController
   * knows nothing about sessions, while the service that owns the capture lifecycle
   * implements both.
   */
  fun onStartVideo(mode: Int, width: Int, height: Int, fps: Int, bitrateBps: Int, rtpPort: Int, explicitStart: Boolean = false): Byte =
    AckResult.Unsupported

  /** Stop a wireless session started by [onStartVideo]. The control channel stays open. */
  fun onStopVideo(userRequested: Boolean = false): Byte = AckResult.Unsupported
}

/**
 * Owns the phonecam_control socket: accepts the PC's connection, sends
 * CapabilityDescriptor immediately (docs/wire-protocol.md's control-channel handshake), then
 * dispatches incoming commands to [listener] and exposes send* methods for telemetry.
 *
 * Durable by design: the channel is owned by CaptureService and keeps serving across capture
 * sessions, so a PC can command a phone that is sitting idle (starting a wireless session, for
 * instance) -- a channel that only existed while a capture was running could never receive the
 * command that starts one. A PC that disconnects and comes back is served by the same
 * listener, without rebinding the socket.
 */
class ControlChannel(private val context: Context, private val listener: ControlCommandListener) {
  private val server = ControlSocketServer()
  private var output: OutputStream? = null
  private val stopped = AtomicBoolean(false)

  // Guards writes to the control socket -- see sendBytes.
  private val sendLock = Any()

  /**
   * The phone's pairing code, used to challenge PCs that arrive over the network. A session
   * over the adb-forwarded abstract socket is a local USB link and is trusted without one.
   */
  var pairingCodeProvider: () -> String = { "" }
  var trustedPcKeyProvider: (String) -> String = { "" }
  var stoppedProvider: () -> Boolean = { false }
  val isConnected: Boolean get() = authenticated && output != null
  val clientId: String get() = activeClientId
  @Volatile var networkLoginAllowed = true
  // True while the phone is scanning a PC's QR and must not accept a network login.
  // Time-limited by the caller, so an abandoned scan cannot lock the phone out.
  var networkLoginBlocked: () -> Boolean = { false }
  @Volatile private var activeClientId = ""
  @Volatile private var activeKey = ""

  fun disconnectClient() {
    networkVideoAuth.clear()
    server.closeClient()
  }

  fun revokeForgottenPc() {
    if (activeClientId.isNotEmpty() && trustedPcKeyProvider(activeClientId) != activeKey) disconnectClient()
  }

  // Whether the current session has proved it may drive the camera. A local (USB) session is
  // trusted from the start; a network session has to answer the challenge first.
  @Volatile private var authenticated = false
  private var authPhoneNonce: String? = null
  private var sessionRejected = false

  // When the unanswered challenge stops being waited for.
  private var authDeadlineMs = 0L
  private val networkVideoAuth = NetworkVideoAuth()

  fun authenticateVideoConnection(socket: Socket, isCancelled: () -> Boolean): Boolean =
    networkVideoAuth.authenticate(socket, isCancelled)

  /**
   * The address of the PC this session is connected to, or null when the connection came
   * through the adb-forwarded abstract socket (where the peer address is meaningless).
   * A wireless session needs a real address to stream to.
   */
  @Volatile
  var peerAddress: String? = null
    private set

  /**
   * Called after each control session ends (the PC disconnected).
   *
   * The control channel is the lifeline of a PC-driven session: when it goes away, the PC is
   * gone, and a capture that only existed because the PC asked for it should stop. That matters
   * most for the wireless transport, where a UDP sink never fails on its own -- the phone would
   * otherwise keep the camera open, streaming into the void, and refuse to start a new session.
   */
  var onSessionEnded: (() -> Unit)? = null

  /**
   * Binds the listeners, then serves control sessions until [isCancelled] (or [stop] is
   * called): accept a PC, send capabilities, run the receive loop until it disconnects, then
   * accept the next one.
   *
   * Blocks -- call it from a background thread. The socket is bound exactly once here (not per
   * session): re-binding the same abstract name while the previous listener still holds it
   * fails with "Address already in use", which is why a per-session bind would be wrong.
   */
  fun runSessions(isCancelled: () -> Boolean = { false }) {
    stopped.set(false)
    server.open(isCancelled)
    while (!isCancelled() && !stopped.get()) {
      val session =
        try {
          server.accept(isCancelled)
        } catch (e: IOException) {
          // A real accept failure (not our own close) shouldn't kill the channel -- the PC
          // simply isn't there yet, and the next accept() keeps waiting.
          if (!isCancelled() && !stopped.get()) {
            Log.w(TAG, "accept() failed, retrying", e)
            continue
          }
          break
        }
      if (stopped.get()) {
        try {
          session.first.close()
          session.second.close()
        } catch (_: IOException) {
          // already torn down
        }
        break
      }
      val (input, out) = session
      networkVideoAuth.clear()
      sessionRejected = false
      output = out
      peerAddress = server.lastAcceptedAddress
      try {
        Log.i(TAG, "control session started (peer=$peerAddress)")

        // A network session must prove it knows the pairing code. The adb-forwarded
        // abstract socket is trusted without one.
        val isLocalUsbSession = server.lastAcceptedAddress == null
        authenticated = isLocalUsbSession
        if (isLocalUsbSession) {
          sendCapabilities()
        } else {
          val nonce = SessionAuth.newNonce()
          authPhoneNonce = nonce
          authDeadlineMs = System.currentTimeMillis() + AUTH_TIMEOUT_MS
          sendAuthChallenge(out, nonce)
        }

        receiveLoop(input) // blocks until the PC disconnects
      } catch (e: IOException) {
        if (!stopped.get()) Log.w(TAG, "control session failed", e)
      } finally {
        networkVideoAuth.clear()
        Log.i(TAG, "control session ended")
        output = null
        peerAddress = null
        authenticated = false
        authPhoneNonce = null
        activeClientId = ""
        activeKey = ""
        server.closeClient()
        // Capture started by this PC ends with its control connection.
        onSessionEnded?.invoke()
      }
    }
  }

  fun stop() {
    networkVideoAuth.clear()
    if (!stopped.compareAndSet(false, true)) return
    server.close() // unblocks a pending accept() or receiveLoop's read
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

  // Periodic telemetry, previously defined in the protocol (proto/control.fbs) but
  // never sent -- CaptureController calls this roughly once a second while streaming (see its
  // statsJob), feeding the adaptive-bitrate policy there and the host console's "Stats" line.
  fun sendStats(encodeFps: Float, bitrateKbps: Int, droppedFrames: Int, thermalStatus: Byte) {
    val builder = FlatBufferBuilder(64)
    val payloadOffset =
      Stats.createStats(builder, encodeFps, bitrateKbps.toUInt(), droppedFrames.toUInt(), thermalStatus)
    val envelope = ControlEnvelope.createControlEnvelope(builder, 0u, ControlPayload.Stats, payloadOffset)
    ControlEnvelope.finishControlEnvelopeBuffer(builder, envelope)
    sendBytes(builder)
  }

  private fun sendCapabilities() {
    sendStreamIntent()
    val builder = FlatBufferBuilder(1024)
    val capsOffset = CameraCapabilityProbe.buildCapabilityDescriptor(context, builder)
    val envelope =
      ControlEnvelope.createControlEnvelope(
        builder, 0u, ControlPayload.phonecam_wire_CapabilityDescriptor, capsOffset,
      )
    ControlEnvelope.finishControlEnvelopeBuffer(builder, envelope)
    sendBytes(builder)
  }

  fun sendStreamIntent() {
    if (!authenticated) return
    val builder = FlatBufferBuilder(64)
    val payload = phonecam.control.StreamIntent.createStreamIntent(builder, stoppedProvider())
    val envelope = ControlEnvelope.createControlEnvelope(builder, 0u, ControlPayload.StreamIntent, payload)
    ControlEnvelope.finishControlEnvelopeBuffer(builder, envelope)
    sendBytes(builder)
  }

  /**
   * Sends the challenge: a fresh nonce the PC has to include (keyed) in its answer.
   */
  private fun sendAuthChallenge(out: OutputStream, nonce: String) {
    val builder = FlatBufferBuilder(128)
    val nonceOffset = builder.createString(nonce)
    val payloadOffset = AuthChallenge.createAuthChallenge(builder, nonceOffset)
    val envelope = ControlEnvelope.createControlEnvelope(builder, 0u, ControlPayload.AuthChallenge, payloadOffset)
    ControlEnvelope.finishControlEnvelopeBuffer(builder, envelope)
    val buf = builder.dataBuffer()
    val bytes = ByteArray(buf.remaining())
    buf.get(bytes)
    ControlFraming.writeEnvelope(out, bytes)
  }

  /**
   * Checks the PC's answer to the challenge. The PC proves it knows the pairing code with an
   * HMAC over both nonces; because both are fresh per session, a recorded exchange cannot be
   * replayed, and the code itself never crosses the wire.
   */
  private fun checkAuthResponse(code: String, pcNonce: String, hmac: String): Boolean {
    val phoneNonce = authPhoneNonce
    if (code.isNullOrEmpty() || phoneNonce == null) return false
    val expected = SessionAuth.computeHmac(code, phoneNonce, pcNonce)
    return SessionAuth.matches(expected, hmac)
  }

  private fun sendError(message: String) {
    val out = output ?: return
    val builder = FlatBufferBuilder(128)
    val messageOffset = builder.createString(message)
    val payloadOffset = Error.createError(builder, 0u, messageOffset)
    val envelope = ControlEnvelope.createControlEnvelope(builder, 0u, ControlPayload.Error, payloadOffset)
    ControlEnvelope.finishControlEnvelopeBuffer(builder, envelope)
    val buf = builder.dataBuffer()
    val bytes = ByteArray(buf.remaining())
    buf.get(bytes)
    ControlFraming.writeEnvelope(out, bytes)
  }

  private fun sendBytes(builder: FlatBufferBuilder) {
    val out = output ?: return
    // Serialized: the channel is written from three different threads -- the control receive
    // thread (Acks), the stats thread (telemetry) and the camera callback thread (lens/settings)
    // -- and each message is a length prefix followed by its body. Two threads interleaving
    // those two writes corrupt the framing, and the PC then decodes garbage: confirmed live,
    // as a control message that arrived with payload type 0 (i.e. no payload at all).
    synchronized(sendLock) {
      // Re-read the stream under the lock: the session can end between the check above and
      // here, and writing to a closed stream would throw.
      val current = output ?: return
      try {
        val buf = builder.dataBuffer()
        val bytes = ByteArray(buf.remaining())
        buf.get(bytes)
        ControlFraming.writeEnvelope(current, bytes)
      } catch (_: IOException) {
        // Best-effort: a send failure means the connection is already broken;
        // the receive loop's own EOF/IO handling is what tears the session down.
      }
    }
  }

  private fun receiveLoop(input: InputStream) {
    try {
      while (!stopped.get() && !sessionRejected) {
        if (!authenticated && System.currentTimeMillis() > authDeadlineMs) break
        val bytes =
          try {
            ControlFraming.readEnvelope(input)
          } catch (_: SocketTimeoutException) {
            // A timeout can occur halfway through a prefix/body. End this session rather
            // than interpreting the remainder as a new envelope.
            Log.w(TAG, "the pairing challenge timed out -- closing the session")
            break
          }
        dispatch(bytes)
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
    // Nothing but the authentication answer is honored before the PC has proved it may drive
    // the camera. A local (USB) session is trusted from the start, so this only ever bites on
    // a network session.
    if (envelope.payloadType != ControlPayload.AuthResponse && !authenticated) {
      sendAck(cmdId, AckResult.Error, "not authorized: answer the pairing challenge first")
      return
    }
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
        ControlPayload.GetStreamIntent -> {
          sendStreamIntent()
          AckResult.Ok
        }
        ControlPayload.StopVideo -> {
          val t = StopVideo()
          envelope.payload(t)
          listener.onStopVideo(t.userRequested)
        }
        ControlPayload.AuthResponse -> {
          val t = AuthResponse()
          envelope.payload(t)
          val clientId = t.clientId ?: ""
          val code = if (clientId.isEmpty()) pairingCodeProvider() else trustedPcKeyProvider(clientId)
          if (networkLoginAllowed && !networkLoginBlocked() && checkAuthResponse(code, t.nonce ?: "", t.hmac ?: "")) {
            server.finishAuthentication()
            authenticated = true
            activeClientId = clientId
            activeKey = code
            val peer = peerAddress
            if (peer != null) {
              networkVideoAuth.startSession(peer, code, authPhoneNonce!!, t.nonce ?: "")
            }
            Log.i(TAG, "the PC proved it knows the pairing code")
            // The capabilities follow the successful authentication: the PC is told what the
            // camera can do only once it is allowed to ask.
            sendCapabilities()
            AckResult.Ok
          } else {
            networkVideoAuth.clear()
            Log.w(TAG, "the PC's authentication answer did not match -- refusing the session")
            sendError("not authorized: wrong pairing code")
            sessionRejected = true
            AckResult.Error
          }
        }
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
        ControlPayload.StartVideo -> {
          val t = StartVideo()
          envelope.payload(t)
          listener.onStartVideo(t.mode.toInt(), t.width.toInt(), t.height.toInt(), t.fps.toInt(),
            t.bitrateBps.toInt(), t.rtpPort.toInt(), t.explicitStart)
        }
        ControlPayload.StopVideo -> {
          val t = StopVideo()
          envelope.payload(t)
          listener.onStopVideo(t.userRequested)
        }
      else -> AckResult.Unsupported
    }

  private companion object {
    const val TAG = "ControlChannel"

    // How long a challenged PC has to answer before the phone gives up on it.
    const val AUTH_TIMEOUT_MS = 10_000L
  }
}
