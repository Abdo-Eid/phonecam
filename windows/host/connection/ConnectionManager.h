#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include "bridge/LiveVideoBridge.h"
#include "control/ControlChannel.h"

namespace phonecam::connection {
enum class Mode { Automatic, Adb, Aoa, Tethered, WifiRtp, WifiTcp, Disconnected };
enum class State { Disconnected, Connecting, Pairing, WaitingForVideo, Streaming, Stopped, Error };
struct Target {
    Mode mode = Mode::Automatic;
    std::string address;
    std::string pairingCode;
    std::string clientId;
    std::string phoneId;
    uint16_t controlPort = 27184;
    uint16_t videoPort = 27183;
    uint16_t rtpPort = 27185;
    bool remember = false;
};
struct Snapshot {
    Mode mode = Mode::Disconnected;
    State state = State::Disconnected;
    std::string address;
    std::string message = "Disconnected";
    bool controlReady = false;
    uint64_t request = 0;
    uint64_t streamingEvent = 0;
};

// Owns reconnect/watchdog and serializes stop/reconfigure/start away from the tray thread.
// The shared ring and virtual camera remain registered throughout a transport change.
class ConnectionManager {
public:
    ConnectionManager() = default;
    ~ConnectionManager();
    bool Start(Target initial, std::function<Target()> resolveAutomatic);
    uint64_t Connect(Target target);
    void Stop();
    void SetStreaming(bool enabled);
    Snapshot GetSnapshot() const;
    void SetMessageHandler(phonecam::control::ControlMessageHandler handler) { onMessage_ = std::move(handler); }
    phonecam::control::ControlChannel& Control() { return control_; }
    phonecam::bridge::LiveVideoBridge& Bridge() { return bridge_; }
private:
    void Run();
    void Publish(State state, const std::string& message, bool ready = false);
    void ControlLoop(Target target);
    void StopSession();
    std::unique_ptr<phonecam::transport::VideoTransport> MakeTransport(const Target& target);
    phonecam::control::ControlChannel control_;
    phonecam::bridge::LiveVideoBridge bridge_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Target> pending_;
    Snapshot snapshot_;
    uint64_t sequence_ = 0;
    std::function<Target()> resolveAutomatic_;
    phonecam::control::ControlMessageHandler onMessage_;
    std::thread worker_;
    std::thread controlWorker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> sessionRunning_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> rememberFailed_{false};
    std::atomic<bool> paused_{false};       // the user stopped it on this PC
    std::atomic<bool> phoneStopped_{false}; // the user stopped it on the phone
    std::atomic<bool> startNeeded_{false};
    std::atomic<int> userAction_{-1};
    std::atomic<bool> resumeRequested_{false};
    std::atomic<int64_t> lastControlMessageMs_{0};
    bool notifyOnStreaming_ = true; // protected by mutex_, rearmed only by user actions
};
std::string ModeName(Mode mode);
}
