#pragma once

// Common video-channel interface for ADB-forwarded TCP and USB-tethered TCP.
// The bridge reconnect loop doesn't need to know how the PC reaches the phone.

#include <cstdint>
#include <functional>

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
    bool discontinuity = false;
};

using VideoPacketCallback = std::function<void(const VideoPacket&)>;

class VideoTransport {
public:
    virtual ~VideoTransport() = default;

    virtual bool Connect() = 0;
    virtual void Disconnect() = 0;

    // Blocks parsing and dispatching packets until the connection closes or a
    // framing error occurs.
    virtual void RunReceiveLoop(const VideoPacketCallback& onPacket) = 0;
};

}  // namespace phonecam::transport
