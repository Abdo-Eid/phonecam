#pragma once

// Encodes/decodes phonecam.control.ControlEnvelope (proto/control.fbs) over
// a ControlTransport. Callers deal only in plain structs (below), never
// FlatBuffers generated types -- keeps ConsoleControlUi and future callers
// decoupled from codegen churn. See docs/control-protocol.md for message
// semantics and the cmd_id/Ack correlation discipline.
//
// No FlatBuffers/winsock types in this header for the same reason
// ControlTransport.h avoids them: keeps include order unconstrained at call
// sites (main.cpp etc).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace phonecam::control {

// The pairing handshake's proof: HMAC-SHA256(code, phone_nonce || pc_nonce), hex-encoded.
// Exposed (rather than kept private to the channel) so the host's --test-auth mode can
// check it against an independent implementation.
std::string ComputeAuthHmac(const std::string& code, const std::string& phoneNonce,
                             const std::string& pcNonce);
std::string ComputeVideoAuthHmac(const std::string& code, const std::string& phoneNonce,
                                 const std::string& pcNonce, const std::string& videoNonce);

struct LensInfo {
    std::string cameraId;
    int facing = 0;  // phonecam.wire.LensFacing: 0=Back 1=Front 2=External
    int hardwareLevel = 0;
    bool hasManualSensor = false;
    int resolutionCount = 0;
    float zoomMin = 1.0f, zoomMax = 1.0f;
    int32_t isoMin = 0, isoMax = 0;
    int64_t exposureTimeNsMin = 0, exposureTimeNsMax = 0;
    int32_t evMin = 0, evMax = 0;
    float evStep = 0.0f;
    std::vector<int> afModes;   // phonecam.wire.AfMode values
    std::vector<int> awbModes;  // phonecam.wire.AwbMode values
    bool hasTorch = false;
    bool hasOpticalStabilization = false;
};

struct CapabilityInfo {
    uint16_t protocolVersion = 0;
    std::string deviceModel;
    std::string androidRelease;
    std::vector<LensInfo> lenses;
};

struct AckInfo {
    uint32_t cmdId = 0;
    int result = 0;  // phonecam.control.AckResult
    std::string message;
};

// The phone's challenge: a fresh nonce the PC has to include, keyed, in its answer.
struct AuthChallengeInfo {
    std::string nonce;
};

struct SettingsInfo {
    std::string cameraId;
    uint16_t width = 0, height = 0, fps = 0;
    uint32_t bitrateBps = 0;
    float zoomRatio = 1.0f;
    int focusMode = 0;
    int32_t evSteps = 0;
    bool manualExposure = false;
    int32_t iso = 0;
    int64_t exposureTimeNs = 0;
    int awbMode = 0;
    uint16_t whiteBalanceTemperatureK = 0;
    bool torchOn = false;
    bool stabilizationOn = false;
};

struct StatsInfo {
    float encodeFps = 0.0f;
    uint32_t bitrateKbps = 0;
    uint32_t droppedFrames = 0;
    int thermalStatus = 0;
};

struct LensChangedInfo {
    std::string cameraId;
};

struct OrientationChangedInfo {
    uint16_t degrees = 0;
};

struct ErrorInfo {
    uint32_t cmdId = 0;
    uint16_t code = 0;
    std::string message;
};

enum class ControlMsgKind {
    Capabilities,
    AuthChallenge,
    Ack,
    Settings,
    Stats,
    AfState,
    AeState,
    LensChanged,
    Orientation,
    Error,
    Unknown,
};

struct ControlMessage {
    ControlMsgKind kind = ControlMsgKind::Unknown;
    CapabilityInfo capabilities;
    AuthChallengeInfo authChallenge;
    AckInfo ack;
    SettingsInfo settings;
    StatsInfo stats;
    int afState = 0;
    int aeState = 0;
    LensChangedInfo lensChanged;
    OrientationChangedInfo orientation;
    ErrorInfo error;
};

using ControlMessageHandler = std::function<void(const ControlMessage&)>;

class ControlChannel {
public:
    ControlChannel();
    ~ControlChannel();

    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;

    // host empty == the adb-forward path (unchanged); host set == connect straight to that
    // address, no adb (USB tethering / Wi-Fi). See ControlTransport::Connect.
    bool Connect(uint16_t port = 27184, const std::string& host = {});
    void Disconnect();

    // The pairing code this host answers the phone's challenge with. Over a network
    // link the phone will not accept commands until the PC proves it knows this
    // code; over the local USB link the phone trusts the connection without one.
    //
    // An empty code means "no code configured": the host then cannot answer a
    // challenge, and a phone that asks for one will refuse the session.
    void SetPairingCode(std::string code);

    // Called when the session is ready for commands -- i.e. when the phone has sent
    // its capabilities, which it only does after a successful authentication. Commands
    // sent before that are rejected by the phone, so anything that wants to start a
    // session should wait for this rather than for the connection itself.
    void SetSessionReadyHandler(std::function<void()> handler);

    // Empty unless the current network control session has completed authentication.
    // Used only at TCP video connect time, never per frame.
    std::string VideoAuthProof(const std::string& challenge) const;

    // Blocks until disconnect; invokes onMessage for each decoded envelope.
    void RunReceiveLoop(const ControlMessageHandler& onMessage);

    // Each Send* assigns the next monotonic, non-zero cmd_id and returns it
    // so the caller can correlate the eventual Ack (docs/control-protocol.md
    // synchronization discipline). Controls that touch camera session
    // lifecycle (SetLens) are included; capability clamping is the caller's
    // responsibility (this class never rejects a value, by design -- see
    // "never send an unsupported command" in the protocol doc).
    uint32_t SendSetZoomRatio(float ratio);
    uint32_t SendSetEv(int32_t steps);
    uint32_t SendSetTorch(bool on);
    uint32_t SendSetFocusMode(int afMode);
    uint32_t SendTapToFocus(float nx, float ny, float regionSize);
    uint32_t SendRequestKeyframe();
    uint32_t SendSetBitrate(uint32_t bitrateBps);
    uint32_t SendSetLens(const std::string& cameraId);

    // Wireless session control (RTP over UDP): asks the phone to start
    // streaming live camera video to this PC. The phone learns the PC's
    // address from the control connection itself, so the only extra
    // information it needs is the UDP port the PC is listening on.
    //
    // Sent on every (re)connect, not once per process: the phone may have
    // restarted, or its capture may have stopped while the control channel
    // stayed up -- either way the phone is the one that knows whether video
    // is actually running, and it answers Ack{Busy} if it already is.
    //
    // [mode] is VideoTransportMode: FramedTcp for the socket-based transports
    // (the phone binds its video socket and the video transport connects to
    // it), Rtp for the UDP one.
    uint32_t SendStartVideo(uint16_t width, uint16_t height, uint16_t fps, uint32_t bitrateBps, uint16_t rtpPort,
                            uint8_t mode = 0);

    // Stops a wireless session (the phone keeps its control channel open).
    uint32_t SendStopVideo();

    // Tray controls: a cache of the last-seen Capabilities/CurrentSettings messages,
    // populated inside RunReceiveLoop before the message reaches the caller's handler -- moved
    // here (rather than duplicating it in the tray, the way ConsoleControlUi has its own private
    // copy) because this is already the single decode chokepoint every consumer reads from, and
    // the tray needs it independent of whatever ConsoleControlUi is doing. Thread-safe: written
    // from the control-receive thread, read from the tray's message-pump thread when it builds
    // the menu.
    CapabilityInfo GetLastCapabilities() const;
    bool HasCapabilities() const;
    SettingsInfo GetLastSettings() const;
    bool HasSettings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::control
