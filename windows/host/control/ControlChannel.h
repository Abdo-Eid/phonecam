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

    bool Connect(uint16_t port = 27184);
    void Disconnect();

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::control
