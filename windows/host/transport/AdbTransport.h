#pragma once

// Manages `adb forward` for the phonecam_video abstract socket and a TCP
// client connection to the forwarded port, per docs/wire-protocol.md. Video-
// channel only for Phase 3 -- the control channel handshake described in
// that doc is Phase 4 scope.
//
// No winsock/windows types in this header on purpose: it's included from
// main.cpp alongside <mfapi.h>, and winsock2.h must be the first Windows
// header included in a translation unit (or windows.h's default winsock.h
// pull-in conflicts with it) -- keeping that entirely inside the .cpp avoids
// depending on include order at every call site.

#include <cstdint>
#include <memory>

#include "transport/VideoTransport.h"

namespace phonecam::transport {

class AdbVideoTransport : public VideoTransport {
public:
    AdbVideoTransport();
    ~AdbVideoTransport() override;

    AdbVideoTransport(const AdbVideoTransport&) = delete;
    AdbVideoTransport& operator=(const AdbVideoTransport&) = delete;

    // Runs `adb forward tcp:27183 localabstract:phonecam_video` (adb located
    // via PATH -- Phase 5 bundles a copy in third_party/platform-tools) and
    // connects as a TCP client to 127.0.0.1:27183, retrying briefly since
    // forward can return before the phone-side listener actually accepts.
    bool Connect() override;
    void Disconnect() override;
    void RunReceiveLoop(const VideoPacketCallback& onPacket) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::transport
