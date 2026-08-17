#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "shm/SharedFrameRing.h"

namespace phonecam::bridge {

// Phase 1b dev/test tool: writes a synthetic animated NV12 pattern into the
// SharedFrameRing so the vcam side (and the whole shm contract) can be
// verified without a real phone connected. Superseded by the real
// H.264-decode bridge in Phase 3, which will write actually-decoded frames
// through the same SharedFrameRing::WriteFrame() call.
class TestPatternProducer {
public:
    ~TestPatternProducer();

    bool Start(uint32_t width, uint32_t height, uint32_t fps);
    void Stop();

private:
    void Run(uint32_t width, uint32_t height, uint32_t fps);

    phonecam::shm::SharedFrameRing ring_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace phonecam::bridge
