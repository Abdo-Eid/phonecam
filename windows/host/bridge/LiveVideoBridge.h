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

    // Opens the SharedFrameRing (phonecam-svc must already be running),
    // initializes the decoder, and connects to the phone -- which requires
    // capture to already be running there (the phone's LocalServerSocket
    // only accepts once the user has started capture). Fails fast rather
    // than retrying/waiting; that ordering + retry robustness is Phase 5
    // scope, same as the rest of the reconnect story in docs/wire-protocol.md.
    bool Start();
    void Stop();

private:
    void Run();

    phonecam::shm::SharedFrameRing ring_;
    phonecam::transport::AdbVideoTransport transport_;
    phonecam::decode::MFH264Decoder decoder_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace phonecam::bridge
