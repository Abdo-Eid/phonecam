#pragma once
#include <cstdint>

// 30 fps in MF's 100 ns clock. Keep the fractional tick instead of drifting
// by using a rounded 333333 duration for every frame.
class SampleClock {
public:
    void Reset(int64_t now) { epoch_ = now; next_ = 0; }
    struct Tick { int64_t time; int64_t duration; };
    Tick Next(int64_t now) {
        const int64_t elapsed = now > epoch_ ? now - epoch_ : 0;
        const int64_t current = elapsed * 30 / 10000000;
        if (current > next_) next_ = current; // skip missed slots, never burst to catch up
        const int64_t time = epoch_ + next_ * 10000000 / 30;
        ++next_;
        return {time, epoch_ + next_ * 10000000 / 30 - time};
    }
private:
    int64_t epoch_ = 0;
    int64_t next_ = 0;
};
