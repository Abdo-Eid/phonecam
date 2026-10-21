#pragma once

// The real frame source, superseding TestPatternProducer as the
// host's default. Connects to the phone over adb-forward (transport/), feeds
// the received Annex-B stream into the MF H.264 decoder (decode/), and
// writes each decoded NV12 frame into the same SharedFrameRing the vcam side
// already reads from -- only the frame source is new; everything
// upstream and downstream of WriteFrame() is unchanged.

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "decode/MFH264Decoder.h"
#include "shm/SharedFrameRing.h"
#include "transport/AdbTransport.h"
#include "transport/VideoTransport.h"

namespace phonecam::bridge {

class LiveVideoBridge {
public:
    // Defaults to the adb-forward transport (the proven path) --
    // pass an AoaVideoTransport (transport/AoaTransport.h) to run the same
    // decode/ring/vcam pipeline over AOA instead. See main.cpp's --aoa flag.
    explicit LiveVideoBridge(std::unique_ptr<phonecam::transport::VideoTransport> transport =
                                  std::make_unique<phonecam::transport::AdbVideoTransport>());
    ~LiveVideoBridge();

    // Opens the SharedFrameRing (phonecam-svc must already be running) and
    // initializes the decoder -- both are treated as fatal setup failures
    // (something structurally wrong, not "phone not ready yet"), so Start()
    // still fails fast for those. The phone-side connection itself is not
    // required to succeed here: a background thread owns connect-or-retry,
    // so Start() returns as soon as the ring/decoder are ready, before the
    // phone has necessarily started capture (reconnect robustness).
    bool Start();
    void Stop();

    // Cheap, thread-safe status snapshot for the tray icon -- true only while the phone
    // connection is actually up, not while the supervise loop is retrying.
    bool IsConnected() const { return connected_.load(); }

    // How many frames have been decoded and handed to the ring since the bridge started.
    // Used to tell "the session is up but nothing is arriving" from "the session is fine" --
    // the two need different reactions (re-ask the phone / leave it alone).
    uint64_t FramesDecoded() const { return framesDecoded_.load(); }

    // Reference-relative timing, not absolute end-to-end latency. Comparing elapsed
    // phone/PC time cancels both their clock offset and the reference frame's delay.
    // This measures changes in delay, not the camera/encoder/transport/decode baseline.
    //
    // The median of a recent window is reported rather than the mean: a single
    // stalled frame (a keyframe, a reconnect) would otherwise dominate the average
    // and hide what the stream actually feels like.
    struct Latency {
        double medianMs = 0;
        double minMs = 0;
        double maxMs = 0;
        uint64_t samples = 0;
    };
    Latency GetLatency() const;

    // Thin forwarders so main.cpp/the tray can reach the ring's standing settings
    // without touching the private ring_ member directly. SetTransform is a no-op (returns false)
    // until Start() has opened the ring; GetNegotiatedSize reports 0,0 the same way until the vcam
    // side has negotiated a size at least once.
    bool SetTransform(phonecam::shm::Rotation rotation, bool mirror) { return ring_.SetTransform(rotation, mirror); }
    void GetNegotiatedSize(uint32_t& width, uint32_t& height) const { ring_.GetNegotiatedSize(width, height); }

private:
    void Run();
    bool WaitBeforeRetry();
    void RecordLatency(uint64_t phoneTimestampUs, uint64_t pcNowUs);

    phonecam::shm::SharedFrameRing ring_;
    std::unique_ptr<phonecam::transport::VideoTransport> transport_;
    phonecam::decode::MFH264Decoder decoder_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> framesDecoded_{0};

    // Latency accounting, written by the receive thread and read by whoever asks.
    // The reference pair is what makes the measurement possible across two unrelated
    // clock domains: it holds the first frame's timestamp on each side, and the constant
    // offset between them cancels out of every later sample.
    //
    // The samples live in a small ring buffer so the reported figure describes the
    // recent past (what the stream feels like now) rather than the whole session.
    static constexpr size_t kLatencyWindow = 120;  // ~4s at 30fps
    mutable std::mutex latencyMutex_;
    double latencySamples_[kLatencyWindow] = {};
    size_t latencySampleCount_ = 0;
    size_t latencySampleIndex_ = 0;
    uint64_t latencyTotalSamples_ = 0;
    bool latencyReferenceSet_ = false;
    uint64_t latencyReferencePhoneUs_ = 0;
    uint64_t latencyReferencePcUs_ = 0;
    uint64_t latencyFramesSinceReference_ = 0;
};

}  // namespace phonecam::bridge
