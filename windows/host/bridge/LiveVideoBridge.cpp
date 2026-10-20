#include "bridge/LiveVideoBridge.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "log/Log.h"

namespace phonecam::bridge {

namespace {
constexpr std::chrono::milliseconds kReconnectPollInterval{200};
constexpr std::chrono::milliseconds kReconnectBackoff{2000};

// How many frames after the latency reference are ignored: the pipeline is still warming
// up (the decoder negotiates its output format, the first keyframe is the expensive one),
// and the reference frame itself measures as exactly zero.
constexpr uint64_t kLatencyWarmupFrames = 10;
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
// (reconnect robustness) -- previously Start() failed permanently if the phone wasn't
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
            uint64_t packets = 0;
            uint64_t frames = 0;
            auto lastReport = std::chrono::steady_clock::now();
            transport_->RunReceiveLoop([this, &packets, &frames, &lastReport](
                                           const phonecam::transport::VideoPacket& pkt) {
                ++packets;
                decoder_.Feed(pkt.payload, pkt.payloadSize, pkt.timestampUs,
                               [this, &frames](const phonecam::decode::DecodedFrame& frame) {
                                   // Latency: the wire carries the frame's capture timestamp
                                   // (the encoder's presentation time), so the difference to now
                                   // is the whole pipeline's cost -- camera, encoder, transport,
                                   // decode. The two clocks are different domains (the phone
                                   // counts from its own boot, the PC from an arbitrary epoch),
                                   // so the measurement is taken relative to a reference
                                   // established from the first frame: the constant offset
                                   // cancels and only the elapsed time is compared.
                                   const auto nowUs = static_cast<uint64_t>(
                                       std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count());
                                   RecordLatency(frame.timestampUs, nowUs);
                                   ring_.WriteFrame(frame.width, frame.height, frame.timestampUs, frame.nv12);
                                   ++frames;
                                   framesDecoded_.fetch_add(1, std::memory_order_relaxed);
                               });
                const auto now = std::chrono::steady_clock::now();
                if (now - lastReport >= std::chrono::seconds(5)) {
                    lastReport = now;
                    const Latency latency = GetLatency();
                    phonecam::log::Info("LiveVideoBridge: " + std::to_string(packets) + " packets, " +
                                         std::to_string(frames) + " frames, latency " +
                                         (latency.samples == 0
                                              ? std::string("n/a")
                                              : std::to_string(static_cast<int>(latency.medianMs)) +
                                                    "ms median (" + std::to_string(static_cast<int>(latency.minMs)) +
                                                    "-" + std::to_string(static_cast<int>(latency.maxMs)) + "ms)"));
                }
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

void LiveVideoBridge::RecordLatency(uint64_t phoneTimestampUs, uint64_t pcNowUs) {
    std::lock_guard<std::mutex> lock(latencyMutex_);
    if (!latencyReferenceSet_ || phoneTimestampUs < latencyReferencePhoneUs_) {
        // The first frame establishes the reference (and the constant offset between the
        // two clocks); a timestamp that went backwards means a new encoder session, which
        // restarts the presentation timestamps -- re-establish the reference there too.
        latencyReferenceSet_ = true;
        latencyReferencePhoneUs_ = phoneTimestampUs;
        latencyReferencePcUs_ = pcNowUs;
        latencyFramesSinceReference_ = 0;
        latencySampleCount_ = 0;
        latencySampleIndex_ = 0;
        return;
    }
    ++latencyFramesSinceReference_;
    // The first frames after the reference are the pipeline warming up (the decoder
    // negotiates its output format, the first keyframe is the big one), and the frame
    // the reference was taken from measures as exactly zero by construction. Skipping
    // the first few keeps the figure about the steady state, which is what a user
    // actually experiences.
    if (latencyFramesSinceReference_ <= kLatencyWarmupFrames) return;
    const double ms = (static_cast<double>(pcNowUs - latencyReferencePcUs_) -
                        static_cast<double>(phoneTimestampUs - latencyReferencePhoneUs_)) /
                       1000.0;
    if (ms < 0.0 || ms > 10000.0) return;  // nonsense (clock jump, wrap): not worth reporting
    latencySamples_[latencySampleIndex_] = ms;
    latencySampleIndex_ = (latencySampleIndex_ + 1) % kLatencyWindow;
    if (latencySampleCount_ < kLatencyWindow) ++latencySampleCount_;
    ++latencyTotalSamples_;
}

LiveVideoBridge::Latency LiveVideoBridge::GetLatency() const {
    std::lock_guard<std::mutex> lock(latencyMutex_);
    Latency out;
    out.samples = latencyTotalSamples_;
    if (latencySampleCount_ == 0) return out;
    // Copy the window out and sort it: the median is the middle sample, which is the
    // figure a viewer would actually perceive.
    std::vector<double> sorted(latencySamples_, latencySamples_ + latencySampleCount_);
    std::sort(sorted.begin(), sorted.end());
    out.medianMs = sorted[sorted.size() / 2];
    out.minMs = sorted.front();
    out.maxMs = sorted.back();
    return out;
}

}  // namespace phonecam::bridge
