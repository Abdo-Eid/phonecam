#include "bridge/LiveVideoBridge.h"

#include "log/Log.h"

namespace phonecam::bridge {

LiveVideoBridge::~LiveVideoBridge() { Stop(); }

bool LiveVideoBridge::Start() {
    if (!ring_.Open()) {
        phonecam::log::Error("LiveVideoBridge: failed to open SharedFrameRing (is phonecam-svc running?)");
        return false;
    }
    if (!decoder_.Initialize()) {
        phonecam::log::Error("LiveVideoBridge: decoder Initialize() failed");
        return false;
    }
    if (!transport_.Connect()) {
        phonecam::log::Error("LiveVideoBridge: failed to connect (is capture running on the phone?)");
        return false;
    }

    running_.store(true);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void LiveVideoBridge::Stop() {
    running_.store(false);
    transport_.Disconnect();  // unblocks the receive loop's blocking recv()
    if (thread_.joinable()) {
        thread_.join();
    }
}

void LiveVideoBridge::Run() {
    transport_.RunReceiveLoop([this](const phonecam::transport::VideoPacket& pkt) {
        decoder_.Feed(pkt.payload, pkt.payloadSize, pkt.timestampUs,
                       [this](const phonecam::decode::DecodedFrame& frame) {
                           ring_.WriteFrame(frame.width, frame.height, frame.timestampUs, frame.nv12);
                       });
    });
    phonecam::log::Info("LiveVideoBridge: receive loop ended");
}

}  // namespace phonecam::bridge
