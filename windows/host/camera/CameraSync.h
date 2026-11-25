#pragma once

// Keeps the phone's camera at the settings the user chose on this PC.
//
// The phone forgets: every capture session starts on the back lens with default zoom, exposure
// and focus, and a lens switch resets the last three again. So the PC remembers what was chosen
// and replays it whenever the phone reports a fresh camera. The same mechanism paces sliders:
// at most one command per control is outstanding, only the newest value follows it, and "Busy"
// (camera still opening) is retried instead of being lost. See docs/control-protocol.md.
//
// Not thread-safe; AppController serializes access. Time is passed in (milliseconds, any
// monotonic epoch), so CameraSyncTests.cpp needs no real clock or phone.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "control/ControlChannel.h"

namespace phonecam::camera {

// Unset means "never chosen": leave that setting to the phone. Torch is deliberately absent --
// a saved setting must never switch a light on by itself.
struct CameraChoice {
    std::optional<std::string> lens;
    std::optional<float> zoom;
    std::optional<int32_t> ev;
    std::optional<int> focus;

    bool operator==(const CameraChoice&) const = default;
};

// How to reach the phone; each returns the command id its Ack will carry.
struct CameraCommands {
    std::function<uint32_t(const std::string&)> setLens;
    std::function<uint32_t(float)> setZoom;
    std::function<uint32_t(int32_t)> setEv;
    std::function<uint32_t(int)> setFocus;
};

class CameraSync {
public:
    explicit CameraSync(CameraCommands commands) : commands_(std::move(commands)) {}

    // Replaces everything remembered (startup), without assuming a phone is there yet.
    void Load(const CameraChoice& choice) { desired_ = choice; }
    // Applies only what the choice sets, leaving the rest as it is (a preset).
    void Apply(const CameraChoice& choice, int64_t nowMs);

    void ChooseLens(const std::string& cameraId, int64_t nowMs);
    void ChooseZoom(float ratio, int64_t nowMs);
    void ChooseEv(int32_t steps, int64_t nowMs);
    void ChooseFocus(int mode, int64_t nowMs);

    void OnMessage(const phonecam::control::ControlMessage& message, int64_t nowMs);

    // Sends whatever is due. streaming: frames are arriving, i.e. the camera can take commands.
    void Pump(bool streaming, int64_t nowMs);

    const CameraChoice& Desired() const { return desired_; }
    const std::string& CurrentLens() const { return currentLens_; }
    // The lens the user is waiting for, so a picker doesn't jump back to the old one meanwhile.
    // Empty when nothing is underway.
    std::string AwaitedLens() const {
        return desired_.lens && (lens_.pending || awaitingLensUntil_ != 0) ? *desired_.lens : std::string();
    }

private:
    struct Slot {
        bool pending = false;
        uint32_t inFlight = 0;
        int64_t sentAt = 0;
        int64_t notBefore = 0;
        int attempts = 0;

        void Request(int64_t when) {
            pending = true;
            attempts = 0;
            notBefore = when;
        }
    };

    void ReplayControls(int64_t when);
    bool OnAck(Slot& slot, const phonecam::control::AckInfo& ack, int64_t nowMs);
    bool Due(Slot& slot, int64_t nowMs);
    static void Sent(Slot& slot, uint32_t id, int64_t nowMs);
    const phonecam::control::LensInfo* FindLens(const std::string& cameraId) const;

    CameraCommands commands_;
    CameraChoice desired_;
    phonecam::control::CapabilityInfo capabilities_;
    bool hasCapabilities_ = false;
    std::string currentLens_;
    Slot lens_, zoom_, ev_, focus_;
    int64_t awaitingLensUntil_ = 0;  // a lens change is underway; other commands would be lost
    int lensAttempts_ = 0;
};

}  // namespace phonecam::camera
