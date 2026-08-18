#pragma once

// Phase 7 production host-side AOA transport: mirrors
// android/.../transport/AoaTransport.kt's framing exactly (see that file's
// doc comment for the full rationale) using libusb against the accessory
// interface, per the already-debugged open/claim sequence in
// windows/tools/aoa_probe/main.cpp's OpenAccessoryInterface() and the
// handshake in its RunHandshake(). Video-channel only: control still runs
// over adb (control/ControlChannel.h) -- see docs/architecture.md's Phase 7
// section for why the control path over AOA isn't wired in yet (only the
// video-tagged frame shape has ever been exercised live).
//
// No libusb types in this header on purpose, matching AdbTransport.h's
// winsock-isolation reasoning: this header is included from main.cpp
// alongside <mfapi.h>, and keeping libusb.h's include entirely inside the
// .cpp avoids depending on include order at every call site.

#include <memory>

#include "transport/VideoTransport.h"

namespace phonecam::transport {

class AoaVideoTransport : public VideoTransport {
public:
    AoaVideoTransport();
    ~AoaVideoTransport() override;

    AoaVideoTransport(const AoaVideoTransport&) = delete;
    AoaVideoTransport& operator=(const AoaVideoTransport&) = delete;

    // If the phone is already enumerated in AOA accessory mode (e.g. a prior
    // Connect() from this or an earlier run already switched it), skips
    // straight to claiming the accessory interface. Otherwise finds it in
    // normal mode, runs the AOA handshake (GET_PROTOCOL + identification
    // strings + ACCESSORY_START), and waits for it to re-enumerate before
    // claiming the interface -- matching aoa_probe's --handshake followed by
    // --test-video, now done as one call. Returns false (not fatal --
    // LiveVideoBridge's supervise loop retries) if no matching device is
    // found or the handshake/claim fails.
    bool Connect() override;
    void Disconnect() override;

    // See VideoTransport.h: parses [1-byte Channel tag][self-delimiting
    // body] frames from a buffer-based accumulator, exactly like
    // AoaTransport.kt's runReader/consumeFrames and aoa_probe's
    // RunTestVideo -- never a read sized to one message, per the pipe-stall
    // bug both of those already document. Channel.Control-tagged frames are
    // skipped (not dispatched anywhere yet, see class doc); any other tag
    // drops one byte and resyncs.
    void RunReceiveLoop(const VideoPacketCallback& onPacket) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::transport
