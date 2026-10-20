#pragma once

// Receives H.264 video as RTP (RFC 6184 packetization) over UDP -- the wireless
// transport's video channel.
//
// Why UDP here, when the USB transports use a framed TCP stream: for a live
// camera, a late frame is worth less than a dropped one. TCP's retransmit and
// head-of-line blocking turn a single lost Wi-Fi frame into a latency spike
// that can outlast a whole GOP; UDP lets the receiver drop what it must and
// ask for a fresh keyframe instead. The control channel stays TCP (see
// control/ControlTransport.h) -- commands are small, and their reliability
// matters more than their latency.
//
// What this class does NOT do: it doesn't decode, and it doesn't buffer
// frames for smoothness. It reassembles RTP packets into complete access
// units (Annex-B) and hands each one straight to the caller -- the same
// VideoPacket shape the other transports produce, so the decode bridge, the
// shared-memory ring and the virtual camera are unchanged.
//
// The phone is the RTP sender and the PC the receiver: the phone learns the
// PC's address from the control connection itself (see proto/control.fbs's
// StartVideo), so nothing has to discover a multicast group.

#include <cstdint>
#include <functional>
#include <memory>

#include "transport/VideoTransport.h"

namespace phonecam::transport {

// The PC-side UDP port the phone streams to. The PC binds it; the phone is
// told it in the StartVideo command.
inline constexpr uint16_t kDefaultRtpPort = 27185;

class RtpVideoTransport : public VideoTransport {
public:
    explicit RtpVideoTransport(uint16_t listenPort = kDefaultRtpPort);
    ~RtpVideoTransport() override;

    RtpVideoTransport(const RtpVideoTransport&) = delete;
    RtpVideoTransport& operator=(const RtpVideoTransport&) = delete;

    // Binds the UDP socket. Succeeds even with no phone streaming yet -- the
    // receive loop then simply waits for the first datagram, which is what
    // lets the virtual camera exist before the phone is up.
    bool Connect() override;
    void Disconnect() override;
    void RunReceiveLoop(const VideoPacketCallback& onPacket) override;

    // Called when a packet gap means the video stream lost reference frames
    // and needs a fresh keyframe to recover. Optional -- without it the
    // stream recovers on the encoder's own keyframe interval instead.
    void SetKeyframeRequester(std::function<void()> requester);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::transport
