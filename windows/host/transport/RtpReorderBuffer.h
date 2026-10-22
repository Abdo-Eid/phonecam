#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace phonecam::transport {

// Delay ONLY across a sequence hole, not every picture. A slightly late UDP
// datagram must not cause a whole GOP to be discarded. Actual loss is still
// passed on as a sequence gap after a finite deadline/capacity bound.
class RtpReorderBuffer {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    using Bytes = std::vector<uint8_t>;
    static constexpr auto kMaxWait = std::chrono::milliseconds(12);
    static constexpr size_t kMaxPackets = 64;

    void Push(const uint8_t* data, size_t size, Time now) {
        if (size < 12 || (data[0] >> 6) != 2 || (data[1] & 0x7f) != 96) return;
        const uint16_t sequence = static_cast<uint16_t>((data[2] << 8) | data[3]);
        const uint32_t ssrc = (uint32_t{data[8]} << 24) | (uint32_t{data[9]} << 16) |
                              (uint32_t{data[10]} << 8) | data[11];
        if (!haveSession_ || ssrc != ssrc_) {
            pending_.clear();
            haveSession_ = true;
            ssrc_ = ssrc;
            expected_ = sequence;
        }
        if (static_cast<uint16_t>(sequence - expected_) >= 0x8000u ||
            std::any_of(pending_.begin(), pending_.end(), [sequence](const Packet& packet) {
                return packet.sequence == sequence;
            })) {
            ++lateOrDuplicate;
            return;
        }
        if (pending_.size() >= kMaxPackets) {
            ++capacityDrops;
            return;
        }
        if (sequence == expected_ && !pending_.empty()) ++latePacketsRecovered;
        pending_.push_back({sequence, Bytes(data, data + size), now});
    }

    std::optional<Bytes> Pop(Time now) {
        if (pending_.empty()) return std::nullopt;
        auto next = std::min_element(pending_.begin(), pending_.end(), [this](const Packet& a, const Packet& b) {
            return static_cast<uint16_t>(a.sequence - expected_) < static_cast<uint16_t>(b.sequence - expected_);
        });
        if (next->sequence != expected_) {
            if (pending_.size() < kMaxPackets && now < *Deadline()) return std::nullopt;
            ++expiredGaps;
        }
        expected_ = static_cast<uint16_t>(next->sequence + 1);
        Bytes bytes = std::move(next->data);
        pending_.erase(next);
        return bytes;
    }

    std::optional<Time> Deadline() const {
        if (pending_.empty()) return std::nullopt;
        const auto first = std::min_element(pending_.begin(), pending_.end(), [](const Packet& a, const Packet& b) {
            return a.arrived < b.arrived;
        });
        // Never extend the wait just because more future packets arrive.
        return first->arrived + kMaxWait;
    }

    size_t Pending() const { return pending_.size(); }
    uint64_t latePacketsRecovered = 0;
    uint64_t lateOrDuplicate = 0;
    uint64_t expiredGaps = 0;
    uint64_t capacityDrops = 0;

private:
    struct Packet { uint16_t sequence; Bytes data; Time arrived; };
    std::vector<Packet> pending_;
    bool haveSession_ = false;
    uint32_t ssrc_ = 0;
    uint16_t expected_ = 0;
};

} // namespace phonecam::transport
