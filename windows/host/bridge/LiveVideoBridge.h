#pragma once

// The real frame source, superseding TestPatternProducer as the
// host's default. Connects to the phone over adb-forward (transport/), feeds
// the received Annex-B stream into the MF H.264 decoder (decode/), and
// writes each decoded NV12 frame into the same SharedFrameRing the vcam side
// already reads from -- only the frame source is new; everything
// upstream and downstream of WriteFrame() is unchanged.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "decode/MFH264Decoder.h"
#include "picture/Framing.h"
#include "shm/SharedFrameRing.h"
#include "transport/AdbTransport.h"
#include "transport/VideoTransport.h"

namespace phonecam::bridge {

// One frame for the preview window: everything the phone sees, half size, not yet rotated or
// mirrored. The window draws the crop on top of it.
struct PreviewImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
    phonecam::picture::Size source;  // the phone's full frame
};

class LiveVideoBridge {
public:
    // Defaults to the adb-forward transport; ConnectionManager can replace it
    // with the TCP transport used over USB tethering.
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
    bool Prepare();
    // Serialized by ConnectionManager after the old session is stopped; ring/virtual camera stay up.
    void SetTransport(std::unique_ptr<phonecam::transport::VideoTransport> transport);
    void ClearFrame();
    void SetOutputEnabled(bool enabled);

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

    // What the user chose. The crop is applied here, to each frame before it is published;
    // rotation, mirror and the offered size reach the virtual camera through the ring's standing
    // transform. May be called before Prepare(): the ring is written once it is open.
    void SetPicture(const phonecam::picture::Picture& picture);
    phonecam::picture::Picture GetPicture() const;
    // How far the phone's picture must be turned to be upright, as the phone reports it. Any thread.
    void SetPhoneRotation(phonecam::shm::Rotation rotation);
    // The phone's current frame and how it is turned to be seen, the user's extra turn included.
    phonecam::picture::View GetView() const;

    // The one size the virtual camera is told to offer (what is in the ring, not a recomputation).
    phonecam::picture::Size GetOfferedSize() const;
    // On exit: the ring outlives this process, and an older host started next would create its
    // camera from the leftover size while publishing uncropped frames. Back to "no size chosen".
    void ReleaseOfferedSize();

    // The video app using the camera, and the size it opened it with.
    struct Consumer {
        bool active = false;
        phonecam::picture::Size size;
    };
    Consumer GetConsumer() const;
    // Call a few times a second. The ring keeps the last app's size forever, even across host
    // restarts, but the vcam writes it again with every frame it draws: clearing it and seeing it
    // come back is how "in use now" is told apart from "was used once".
    void PollConsumer();

    // Turns the preview on, or off with nullptr. onFrame runs on the decode thread and must not
    // block; the next image is only produced after TakePreview() collected the previous one, so a
    // busy window drops preview frames and never delays the camera. Costs nothing while off.
    void SetPreview(std::function<void()> onFrame);
    bool TakePreview(PreviewImage& image);

private:
    void Run();
    bool WaitBeforeRetry();
    void RecordLatency(uint64_t phoneTimestampUs, uint64_t pcNowUs);
    void PublishFrame(const phonecam::decode::DecodedFrame& frame);
    void PublishPreview(const phonecam::decode::DecodedFrame& frame);
    phonecam::picture::View ViewLocked() const;
    void UpdateRingLocked();

    phonecam::shm::SharedFrameRing ring_;
    bool ringOpen_ = false;
    std::mutex outputMutex_;
    bool outputEnabled_ = true;
    // Guarded together: they all feed the one value the virtual camera reads.
    mutable std::mutex pictureMutex_;
    phonecam::picture::Picture picture_;
    // Until the phone says otherwise: held upright, filming with a back lens.
    phonecam::shm::Rotation phoneRotation_ = phonecam::shm::Rotation::Deg90;
    phonecam::picture::Size source_{1920, 1080};
    bool released_ = false;
    std::vector<uint8_t> cropBuffer_;  // decode thread only

    std::atomic<uint64_t> consumerSize_{0};
    std::atomic<int64_t> consumerSeenMs_{0};

    std::atomic<bool> previewOn_{false};
    std::mutex previewMutex_;
    std::function<void()> onPreview_;
    PreviewImage preview_;
    bool previewReady_ = false;
    uint64_t previewAtNs_ = 0;
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
