#include <chrono>
#include <iostream>
#include <stdexcept>
#include "../../vcam/SampleClock.h"

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void TestClock() {
    SampleClock clock;
    clock.Reset(100);
    int64_t previous = -1, total = 0;
    for (int i = 0; i < 300; ++i) {
        const auto tick = clock.Next(100);
        Check(tick.time > previous, "sample times must increase, even for queued requests");
        Check(tick.duration == 333333 || tick.duration == 333334, "30 fps duration");
        previous = tick.time;
        total += tick.duration;
    }
    Check(total == 100000000, "300 samples must span exactly ten seconds");
    const auto late = clock.Next(200000100);
    Check(late.time == 200000100, "late request skips missed frames");
    Check(clock.Next(200000100).time > late.time, "no catch-up burst");
    clock.Reset(900);
    Check(clock.Next(900).time == 900, "restart resets the clock");
}

}

int main() {
    try {
        TestClock();
        std::cout << "Video pacing tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
