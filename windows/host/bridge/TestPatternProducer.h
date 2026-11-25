#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "shm/SharedFrameRing.h"

namespace phonecam::bridge {

// Dev/test tool: writes a synthetic animated NV12 pattern into the
// SharedFrameRing so the vcam side (and the whole shm contract) can be
// verified without a real phone connected. Superseded by the real
// H.264-decode bridge, which writes actually-decoded frames
// through the same SharedFrameRing::WriteFrame() call.
class TestPatternProducer {
public:
    ~TestPatternProducer();

    // outputWidth/Height: what the virtual camera should offer (0 = its built-in list).
    bool Start(uint32_t width, uint32_t height, uint32_t fps, uint32_t outputWidth = 0, uint32_t outputHeight = 0);
    void Stop();
    // See LiveVideoBridge::ReleaseOfferedSize.
    void ReleaseOutputSize() { ring_.SetTransform(phonecam::shm::Rotation::Deg0, false); }

private:
    void Run(uint32_t width, uint32_t height, uint32_t fps);

    phonecam::shm::SharedFrameRing ring_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace phonecam::bridge
