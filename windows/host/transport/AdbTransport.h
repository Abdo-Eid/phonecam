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
#include <functional>
#include <memory>

namespace phonecam::transport {

enum class PacketType : uint8_t { Config = 0, Frame = 1 };

// payload is only valid for the duration of the VideoPacketCallback call.
struct VideoPacket {
    PacketType type;
    bool keyframe;
    uint32_t seq;
    uint64_t timestampUs;
    const uint8_t* payload;
    size_t payloadSize;
};

using VideoPacketCallback = std::function<void(const VideoPacket&)>;

class AdbVideoTransport {
public:
    AdbVideoTransport();
    ~AdbVideoTransport();

    AdbVideoTransport(const AdbVideoTransport&) = delete;
    AdbVideoTransport& operator=(const AdbVideoTransport&) = delete;

    // Runs `adb forward tcp:<port> localabstract:phonecam_video` (adb
    // located via PATH -- Phase 5 bundles a copy in third_party/platform-tools)
    // and connects as a TCP client to 127.0.0.1:<port>, retrying briefly since
    // forward can return before the phone-side listener actually accepts.
    bool Connect(uint16_t port = 27183);
    void Disconnect();

    // Blocks parsing and dispatching packets until the connection closes
    // (e.g. the phone stops capture) or a framing error occurs.
    void RunReceiveLoop(const VideoPacketCallback& onPacket);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::transport
