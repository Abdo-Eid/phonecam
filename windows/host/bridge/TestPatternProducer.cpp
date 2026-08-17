#include "bridge/TestPatternProducer.h"

#include <chrono>
#include <cmath>
#include <vector>

#include "log/Log.h"

namespace phonecam::bridge {

TestPatternProducer::~TestPatternProducer() { Stop(); }

bool TestPatternProducer::Start(uint32_t width, uint32_t height, uint32_t fps) {
    // phonecam-svc (a LocalSystem service) owns ring *creation* -- it needs
    // SeCreateGlobalPrivilege, which this normal interactive process doesn't
    // have (see docs/architecture.md). Attaching to an already-existing
    // ring via Open() is a plain DACL check and needs no elevation.
    if (!ring_.Open()) {
        phonecam::log::Error("TestPatternProducer: failed to open SharedFrameRing (is phonecam-svc running?)");
        return false;
    }
    running_ = true;
    thread_ = std::thread(&TestPatternProducer::Run, this, width, height, fps);
    return true;
}

void TestPatternProducer::Stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    ring_.Close();
}

void TestPatternProducer::Run(uint32_t width, uint32_t height, uint32_t fps) {
    const size_t ySize = static_cast<size_t>(width) * height;
    const size_t uvSize = ySize / 2;
    std::vector<uint8_t> nv12(ySize + uvSize);

    const auto frameInterval = std::chrono::milliseconds(1000 / (fps ? fps : 30));
    uint64_t frameIndex = 0;
    const auto start = std::chrono::steady_clock::now();

    while (running_) {
        // Y plane: a bright bar that scrolls left-to-right over time on a
        // dim gradient background, so motion is unambiguous even from a
        // single still frame.
        const uint32_t barX = static_cast<uint32_t>((frameIndex * 6) % width);
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* row = nv12.data() + static_cast<size_t>(y) * width;
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t dist = (x > barX) ? (x - barX) : (barX - x);
                row[x] = (dist < 20) ? 235 : static_cast<uint8_t>(16 + (x * 200 / width));
            }
        }

        // UV plane: a slow color cycle, uniform across the frame.
        const double huePhase = static_cast<double>(frameIndex % 180) / 180.0 * 2.0 * 3.14159265;
        const uint8_t u = static_cast<uint8_t>(128 + 100 * std::sin(huePhase));
        const uint8_t v = static_cast<uint8_t>(128 + 100 * std::cos(huePhase));
        uint8_t* uvPlane = nv12.data() + ySize;
        for (size_t i = 0; i + 1 < uvSize; i += 2) {
            uvPlane[i] = u;
            uvPlane[i + 1] = v;
        }

        const auto now = std::chrono::steady_clock::now();
        const uint64_t timestampUs =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - start).count());
        ring_.WriteFrame(width, height, timestampUs, nv12.data());

        ++frameIndex;
        std::this_thread::sleep_for(frameInterval);
    }
}

}  // namespace phonecam::bridge
