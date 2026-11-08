#include "connection/ConnectionManager.h"
#include <chrono>
#include "bench/MediaSettings.h"
#include "transport/NetTransport.h"
#include "log/Log.h"

namespace phonecam::connection {
namespace {
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

std::string ModeName(Mode mode) {
    switch (mode) {
        case Mode::Automatic: return "USB (automatic)";
        case Mode::Adb: return "USB debugging (ADB)";
        case Mode::Tethered: return "USB tethering";
        default: return "Disconnected";
    }
}

ConnectionManager::~ConnectionManager() { Stop(); }

bool ConnectionManager::Start(Target initial, std::function<Target()> resolveAutomatic) {
    if (!bridge_.Prepare()) return false;
    resolveAutomatic_ = std::move(resolveAutomatic);
    running_.store(true);
    Connect(std::move(initial));
    worker_ = std::thread([this]() { Run(); });
    return true;
}

uint64_t ConnectionManager::Connect(Target target) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(target);
    snapshot_.request = ++sequence_;
    snapshot_.state = State::Connecting;
    snapshot_.controlReady = false;
    snapshot_.message = "Switching connection...";
    notifyOnStreaming_ = true;
    changed_.notify_all();
    return sequence_;
}

Snapshot ConnectionManager::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void ConnectionManager::SetStreaming(bool enabled) {
    if (!ready_.load()) return;
    userAction_.store(enabled ? 1 : 0);
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled) notifyOnStreaming_ = true;
}

void ConnectionManager::Publish(State state, const std::string& message, bool ready) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) return; // a retiring session must not overwrite a newly requested status
        changed = snapshot_.state != state || snapshot_.message != message;
        snapshot_.state = state;
        snapshot_.message = message;
        snapshot_.controlReady = ready;
        if (state == State::Streaming && notifyOnStreaming_) {
            ++snapshot_.streamingEvent;
            notifyOnStreaming_ = false;
        }
    }
    if (changed) phonecam::log::Info("Connection: " + message);
}

void ConnectionManager::Stop() {
    running_.store(false);
    changed_.notify_all();
    control_.Disconnect();
    if (worker_.joinable()) worker_.join();
}

void ConnectionManager::StopSession() {
    sessionRunning_.store(false);
    const bool wasReady = ready_.exchange(false);
    if (wasReady) control_.SendStopVideo();
    control_.Disconnect();
    bridge_.Stop();
    if (controlWorker_.joinable()) controlWorker_.join();
    connected_.store(false);
    control_.ClearCaches();
    bridge_.ClearFrame();
}

std::unique_ptr<phonecam::transport::VideoTransport> ConnectionManager::MakeTransport(const Target& target) {
    using namespace phonecam::transport;
    if (target.mode == Mode::Tethered) {
        auto net = std::make_unique<NetVideoTransport>(target.address);
        net->SetPort(target.videoPort);
        return net;
    }
    return std::make_unique<AdbVideoTransport>();
}

void ConnectionManager::ControlLoop(Target target) {
    while (sessionRunning_.load() && !failed_.load()) {
        std::string address = target.address;
        if (target.mode == Mode::Tethered && address.empty()) address = phonecam::transport::DiscoverTetheredPhoneAddress();
        if (target.mode != Mode::Tethered || !address.empty()) {
            if (control_.Connect(target.controlPort, address)) {
                if (!sessionRunning_.load()) { control_.Disconnect(); break; }
                connected_.store(true);
                Publish(State::Connecting, "Connected to phone; reading camera capabilities...");
                control_.RunReceiveLoop([&](const phonecam::control::ControlMessage& message) {
                    lastControlMessageMs_.store(NowMs());
                    if (message.kind == phonecam::control::ControlMsgKind::StreamIntent) {
                        // The phone's own Stop is authoritative for the camera; this PC's
                        // Stop is authoritative for this PC. Both must gate the output, and
                        // only an explicit resume on either side may re-open it.
                        const bool wasStopped = phoneStopped_.exchange(message.stopped);
                        if (wasStopped != message.stopped) {
                            if (message.stopped) {
                                bridge_.SetOutputEnabled(false);
                            } else {
                                // The user resumed on the phone: an explicit Start, so it also
                                // lifts this PC's pause and asks for video again.
                                paused_.store(false);
                                bridge_.SetOutputEnabled(true);
                                resumeRequested_.store(true);
                                std::lock_guard<std::mutex> lock(mutex_);
                                notifyOnStreaming_ = true;
                            }
                        }
                    } else if (message.kind == phonecam::control::ControlMsgKind::Capabilities &&
                                message.capabilities.protocolVersion != 8) {
                        failed_.store(true);
                        Publish(State::Error, "App versions differ. Update PhoneCam on the phone and laptop.");
                    }
                    if (onMessage_) onMessage_(message);
                });
                ready_.store(false);
                connected_.store(false);
                control_.Disconnect();
                if (!failed_.load()) Publish(State::Connecting, "Connection lost; reconnecting...");
            }
        }
        for (int i = 0; i < 20 && sessionRunning_.load() && !failed_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void ConnectionManager::Run() {
    const auto requestedMedia = phonecam::bench::RequestedMediaSettings();
    if (!requestedMedia) {
        Publish(State::Error, "Invalid debug media benchmark profile");
        return;
    }
    const auto media = *requestedMedia;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running_.load()) {
        Target target;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&]() { return pending_.has_value() || !running_.load(); });
            if (!running_.load()) break;
            target = std::move(*pending_);
            pending_.reset();
        }
        StopSession();
        if (target.mode == Mode::Automatic) target = resolveAutomatic_();
        if (!running_.load()) break;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_) continue;
            snapshot_.mode = target.mode;
            snapshot_.address = target.address;
        }
        if (target.mode == Mode::Disconnected) {
            Publish(State::Disconnected, "Disconnected");
            continue;
        }
        ready_.store(false);
        failed_.store(false);
        userAction_.store(-1);
        startNeeded_.store(false);
        sessionRunning_.store(true);
        control_.SetSessionReadyHandler([this, target, media]() {
            if (!sessionRunning_.load()) return;
            ready_.store(true);
            lastControlMessageMs_.store(NowMs());
            // The phone publishes its stop intent before capabilities, so by now the host
            // knows whether the user stopped it there: never auto-start a stopped stream.
            const bool stopped = paused_.load() || phoneStopped_.load();
            if (!stopped)
                control_.SendStartVideo(media.width, media.height, media.fps, media.bitrate);
            if (!stopped) control_.SendRequestKeyframe();
            Publish(stopped ? State::Stopped : State::WaitingForVideo,
                    stopped ? "Stopped - press Start to resume" : "Connecting - waiting for video", true);
        });
        bridge_.SetTransport(MakeTransport(target));
        if (!bridge_.Start()) {
            sessionRunning_.store(false);
            Publish(State::Error, "Could not start video. Check the PhoneCam service.");
            continue;
        }
        Publish(State::Connecting, "Connecting via " + ModeName(target.mode) + "...");
        controlWorker_ = std::thread([this, target]() { ControlLoop(target); });
        uint64_t lastFrames = 0;
        auto lastChange = std::chrono::steady_clock::now();
        auto lastRequest = lastChange;
        auto lastHeartbeat = lastChange;
        const auto began = lastChange;
        while (running_.load()) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (changed_.wait_for(lock, std::chrono::milliseconds(200), [&]() {
                        return pending_.has_value() || !running_.load();
                    })) break;
            }
            if (failed_.load()) continue;
            const auto now = std::chrono::steady_clock::now();
            // A stopped stream has no video/stats to expose a dead peer. Probe application
            // liveness too, so a silently killed phone cannot leave control "ready" forever.
            if (ready_.load() && NowMs() - lastControlMessageMs_.load() > 6000) {
                ready_.store(false);
                control_.Disconnect();
            }
            if (ready_.load() && now - lastHeartbeat >= std::chrono::seconds(2)) {
                control_.SendGetStreamIntent();
                lastHeartbeat = now;
            }
            const int action = userAction_.exchange(-1);
            if (action == 0) {
                paused_.store(true);
                bridge_.SetOutputEnabled(false);
                control_.SendStopVideo(true);
            } else if (action == 1 || resumeRequested_.exchange(false)) {
                paused_.store(false);
                bridge_.SetOutputEnabled(!phoneStopped_.load());
                control_.SendStartVideo(media.width, media.height, media.fps, media.bitrate, true);
                control_.SendRequestKeyframe();
                lastRequest = now;
                lastChange = now - std::chrono::seconds(3);
            }
            if (paused_.load() || phoneStopped_.load()) {
                Publish(State::Stopped, "Stopped - press Start to resume", ready_.load());
                lastFrames = bridge_.FramesDecoded();
                lastChange = now - std::chrono::seconds(3);
                continue;
            }
            if (startNeeded_.exchange(false)) lastRequest = now - std::chrono::seconds(3);
            const auto frames = bridge_.FramesDecoded();
            if (frames != lastFrames) { lastFrames = frames; lastChange = now; }
            if (frames > 0 && now - lastChange < std::chrono::seconds(3) && ready_.load()) {
                Publish(State::Streaming, "Streaming via " + ModeName(target.mode), ready_.load());
            } else if (ready_.load()) {
                Publish(State::WaitingForVideo, "Connected; waiting for video. Unlock and open PhoneCam if needed.", true);
                if (now - lastRequest >= std::chrono::seconds(3)) {
                    control_.SendStartVideo(media.width, media.height, media.fps, media.bitrate);
                    control_.SendRequestKeyframe();
                    lastRequest = now;
                }
            } else if (connected_.load() && now - began > std::chrono::seconds(12)) {
                Publish(State::Connecting, "Connected; waiting for camera capabilities...");
            } else if (!connected_.load()) {
                Publish(State::Connecting, "Waiting for phone via " + ModeName(target.mode) + "...");
            }
        }
    }
    StopSession();
    if (SUCCEEDED(apartment)) CoUninitialize();
}
}
