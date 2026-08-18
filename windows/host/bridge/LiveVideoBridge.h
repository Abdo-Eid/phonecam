#pragma once

// Phase 3C: the real frame source, superseding TestPatternProducer as the
// host's default. Connects to the phone over adb-forward (transport/), feeds
// the received Annex-B stream into the MF H.264 decoder (decode/), and
// writes each decoded NV12 frame into the same SharedFrameRing the vcam side
// already reads from (Phase 1) -- only the frame source is new; everything
// upstream and downstream of WriteFrame() is unchanged.

#include <atomic>
#include <thread>

#include "decode/MFH264Decoder.h"
#include "shm/SharedFrameRing.h"
#include "transport/AdbTransport.h"

namespace phonecam::bridge {

class LiveVideoBridge {
public:
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

private:
    void Run();
    bool WaitBeforeRetry();

    phonecam::shm::SharedFrameRing ring_;
    phonecam::transport::AdbVideoTransport transport_;
    phonecam::decode::MFH264Decoder decoder_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace phonecam::bridge
