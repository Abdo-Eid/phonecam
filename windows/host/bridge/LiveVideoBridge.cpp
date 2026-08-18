#include "bridge/LiveVideoBridge.h"

#include <chrono>

#include "log/Log.h"

namespace phonecam::bridge {

namespace {
constexpr std::chrono::milliseconds kReconnectPollInterval{200};
constexpr std::chrono::milliseconds kReconnectBackoff{2000};
}  // namespace

LiveVideoBridge::LiveVideoBridge(std::unique_ptr<phonecam::transport::VideoTransport> transport)
    : transport_(std::move(transport)) {}

LiveVideoBridge::~LiveVideoBridge() { Stop(); }

bool LiveVideoBridge::Start() {
    if (!ring_.Open()) {
        phonecam::log::Error("LiveVideoBridge: failed to open SharedFrameRing (is phonecam-svc running?)");
        return false;
    }
    if (!decoder_.Initialize()) {
        phonecam::log::Error("LiveVideoBridge: decoder Initialize() failed");
        return false;
    }

    running_.store(true);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void LiveVideoBridge::Stop() {
    running_.store(false);
    transport_->Disconnect();  // unblocks a pending connect retry or the receive loop's blocking recv()
    if (thread_.joinable()) {
        thread_.join();
    }
}

// Sleeps up to kReconnectBackoff, checking running_ every kReconnectPollInterval so Stop() isn't
// stuck waiting out a full backoff. Returns false if running_ went false while waiting (caller
// should stop, not retry).
bool LiveVideoBridge::WaitBeforeRetry() {
    for (auto waited = std::chrono::milliseconds{0}; waited < kReconnectBackoff && running_.load();
         waited += kReconnectPollInterval) {
        std::this_thread::sleep_for(kReconnectPollInterval);
    }
    return running_.load();
}

// Supervises the phone connection for as long as running_ is set: connect
// (which itself re-runs `adb forward` -- required, since the forward rule
// dies with the USB link, not just the socket), stream until the receive
// loop ends for any reason (phone stopped capture, USB replug, adb-server
// restart), then back off and try again. This is what lets phonecam-host
// survive unplug/replug and phone-side backgrounding without a restart
// (Phase 5) -- previously Start() failed permanently if the phone wasn't
// already streaming, and any later disconnect was terminal.
//
// The backoff applies after EVERY disconnect, not just an outright Connect()
// failure -- confirmed live that `adb forward`'s local TCP proxy can accept
// the PC-side connect() immediately and only then discover (near-instantly)
// that nothing is listening on the phone's abstract socket, so "connected"
// followed by an instant "receive loop ended" is a real, common case, not
// just a hypothetical. Without backing off there too, that case busy-loops:
// no sleep, `adb forward` (a whole new adb.exe process per attempt) launched
// many times a second, observed live before this fix.
//
// Known limitation, logged rather than silently spun through: if the phone
// app is sitting in its own Idle screen (nobody bound to the video socket),
// reconnect attempts here will keep failing until the user taps "Start
// capture" again -- there's no PC->phone wake command for that yet.
void LiveVideoBridge::Run() {
    while (running_.load()) {
        if (!transport_->Connect()) {
            phonecam::log::Error("LiveVideoBridge: connect failed, will retry (is capture running on the phone?)");
        } else {
            connected_.store(true);
            transport_->RunReceiveLoop([this](const phonecam::transport::VideoPacket& pkt) {
                decoder_.Feed(pkt.payload, pkt.payloadSize, pkt.timestampUs,
                               [this](const phonecam::decode::DecodedFrame& frame) {
                                   ring_.WriteFrame(frame.width, frame.height, frame.timestampUs, frame.nv12);
                               });
            });
            connected_.store(false);
            transport_->Disconnect();
            if (!running_.load()) break;
            phonecam::log::Info("LiveVideoBridge: receive loop ended, reconnecting");
        }
        if (!WaitBeforeRetry()) break;
    }
    phonecam::log::Info("LiveVideoBridge: supervise loop stopped");
}

}  // namespace phonecam::bridge
