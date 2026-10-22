#include "transport/RtpReorderBuffer.h"

#include <iostream>
#include <stdexcept>

using namespace phonecam::transport;
using namespace std::chrono_literals;

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<uint8_t> Packet(uint16_t sequence, uint8_t ssrc = 1) {
    return {0x80, 96, static_cast<uint8_t>(sequence >> 8), static_cast<uint8_t>(sequence),
            0,0,0,0, 0,0,0,ssrc, 0x41,0x80};
}
void Push(RtpReorderBuffer& buffer, uint16_t sequence, RtpReorderBuffer::Time time, uint8_t ssrc = 1) {
    const auto bytes = Packet(sequence, ssrc);
    buffer.Push(bytes.data(), bytes.size(), time);
}
uint16_t Pop(RtpReorderBuffer& buffer, RtpReorderBuffer::Time time) {
    const auto packet = buffer.Pop(time);
    Check(packet.has_value(), "expected packet available");
    return static_cast<uint16_t>(((*packet)[2] << 8) | (*packet)[3]);
}
}

int main() {
    try {
        const RtpReorderBuffer::Time start{};
        {
            RtpReorderBuffer buffer;
            Push(buffer, 10, start);
            Check(Pop(buffer, start) == 10 && !buffer.Deadline(), "in-order delivery is immediate");
            Push(buffer, 12, start + 1ms);
            Check(!buffer.Pop(start + 1ms), "hold only across sequence gap");
            Push(buffer, 11, start + 5ms);
            Check(Pop(buffer, start + 5ms) == 11 && Pop(buffer, start + 5ms) == 12, "reorder without declaring loss");
            Check(buffer.latePacketsRecovered == 1 && buffer.expiredGaps == 0, "reorder counters");
        }
        {
            RtpReorderBuffer buffer;
            Push(buffer, 10, start); Pop(buffer, start);
            Push(buffer, 12, start + 1ms);
            Push(buffer, 13, start + 10ms);
            Check(!buffer.Pop(start + 12ms), "deadline not early");
            Check(Pop(buffer, start + 13ms) == 12, "release at deadline without any new arrival");
            Check(Pop(buffer, start + 13ms) == 13 && buffer.expiredGaps == 1, "future arrivals do not extend deadline");
            Push(buffer, 11, start + 14ms);
            Check(!buffer.Pop(start + 14ms) && buffer.lateOrDuplicate == 1, "ignore packets after declared loss");
        }
        {
            RtpReorderBuffer buffer;
            Push(buffer, 65534, start); Pop(buffer, start);
            Push(buffer, 0, start + 1ms);
            Push(buffer, 65535, start + 2ms);
            Check(Pop(buffer, start + 2ms) == 65535 && Pop(buffer, start + 2ms) == 0, "sequence wrap reorders correctly");
            Push(buffer, 0, start + 3ms);
            Check(buffer.Pending() == 0 && buffer.lateOrDuplicate == 1, "duplicate does not start a new gap");
        }
        {
            RtpReorderBuffer buffer;
            Push(buffer, 1, start); Pop(buffer, start);
            for (uint16_t seq = 3; seq < 103; ++seq) Push(buffer, seq, start + 1ms);
            Check(buffer.Pending() == RtpReorderBuffer::kMaxPackets, "capacity bounded despite sender burst");
            Check(Pop(buffer, start + 1ms) == 3 && buffer.expiredGaps == 1, "capacity forces bounded-latency release");
            Check(buffer.capacityDrops == 36, "overflow reported");
        }
        {
            RtpReorderBuffer buffer;
            Push(buffer, 1, start); Pop(buffer, start);
            Push(buffer, 3, start + 1ms);
            Push(buffer, 3, start + 2ms);
            Check(buffer.Pending() == 1 && buffer.lateOrDuplicate == 1, "pending duplicates do not fill buffer");
            Push(buffer, 500, start + 3ms, 2);
            Check(Pop(buffer, start + 3ms) == 500 && !buffer.Deadline(), "new SSRC drops old queued session");
        }
        std::cout << "RTP bounded reordering: PASS (in-order, late, loss deadline, wrap, duplicate, capacity, session)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
