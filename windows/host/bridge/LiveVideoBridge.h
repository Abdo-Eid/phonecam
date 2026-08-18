#pragma once

// Phase 3C: the real frame source, superseding TestPatternProducer as the
// host's default. Connects to the phone over adb-forward (transport/), feeds
// the received Annex-B stream into the MF H.264 decoder (decode/), and
// writes each decoded NV12 frame into the same SharedFrameRing the vcam side
// already reads from (Phase 1) -- only the frame source is new; everything
// upstream and downstream of WriteFrame() is unchanged.

#include <atomic>
#include <memory>
#include <thread>

#include "decode/MFH264Decoder.h"
#include "shm/SharedFrameRing.h"
#include "transport/AdbTransport.h"
#include "transport/VideoTransport.h"

namespace phonecam::bridge {

class LiveVideoBridge {
public:
    // Defaults to the adb-forward transport (Phase 3-5's proven path) --
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
    // phone has necessarily started capture (Phase 5: reconnect robustness).
    bool Start();
    void Stop();

    // Cheap, thread-safe status snapshot for the tray icon (Phase 5) -- true only while the phone
    // connection is actually up, not while the supervise loop is retrying.
    bool IsConnected() const { return connected_.load(); }

private:
    void Run();
    bool WaitBeforeRetry();

    phonecam::shm::SharedFrameRing ring_;
    std::unique_ptr<phonecam::transport::VideoTransport> transport_;
    phonecam::decode::MFH264Decoder decoder_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
};

}  // namespace phonecam::bridge
