#pragma once

// Common video-channel interface both AdbVideoTransport (adb-forward TCP,
// transport/AdbTransport.h) and AoaVideoTransport (libusb AOA accessory pipe,
// transport/AoaTransport.h) implement, so bridge/LiveVideoBridge.cpp's
// connect/receive/reconnect supervising loop doesn't need to know which one
// it's driving -- see LiveVideoBridge::Run()'s comment for why that loop's
// shape (retry Connect() after any disconnect, back off between attempts)
// already fits AOA too: a dropped accessory session has no clean
// reconnect-in-place the way `adb forward` does, so repeated failed
// Connect() calls (logged, backed off) are the correct behavior until the
// user physically replugs -- not a bug to work around here.

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
