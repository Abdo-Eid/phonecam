#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "transport/RtpVideoTransport.h"

namespace {
using namespace phonecam::transport;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Received {
    uint64_t timestamp;
    bool keyframe;
    bool discontinuity;
    std::vector<uint8_t> payload;
};

class Stream {
public:
    Stream() {
        WSADATA wsa{};
        Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Winsock startup");
        tx_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        destination_.sin_family = AF_INET;
        destination_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(bind(tx_, reinterpret_cast<sockaddr*>(&destination_), sizeof(destination_)) == 0, "test bind");
        int size = sizeof(destination_);
        Check(getsockname(tx_, reinterpret_cast<sockaddr*>(&destination_), &size) == 0, "test port");
        const uint16_t port = ntohs(destination_.sin_port);
        // The sender uses another ephemeral port, leaving this one for the receiver.
        closesocket(tx_);
        tx_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        receiver_ = std::make_unique<RtpVideoTransport>(port);
        receiver_->SetKeyframeRequester([this] {
            std::lock_guard lock(mutex_);
            ++requests;
            changed_.notify_all();
        });
        Check(receiver_->Connect(), "receiver bind");
        worker_ = std::thread([this] {
            receiver_->RunReceiveLoop([this](const VideoPacket& packet) {
                if (packet.type != PacketType::Frame) return;
                std::lock_guard lock(mutex_);
                frames_.push_back({packet.timestampUs, packet.keyframe, packet.discontinuity,
                                   {packet.payload, packet.payload + packet.payloadSize}});
                changed_.notify_all();
            });
        });
    }

    ~Stream() {
        receiver_->Disconnect();
        if (worker_.joinable()) worker_.join();
        closesocket(tx_);
        receiver_.reset();
        WSACleanup();
    }

    void Send(uint32_t timestamp, std::vector<uint8_t> payload, bool marker = true, bool hold = false) {
        const uint16_t seq = ++sequence;
        std::vector<uint8_t> data = {0x80, static_cast<uint8_t>(96 | (marker ? 0x80 : 0)),
            static_cast<uint8_t>(seq >> 8), static_cast<uint8_t>(seq),
            static_cast<uint8_t>(timestamp >> 24), static_cast<uint8_t>(timestamp >> 16),
            static_cast<uint8_t>(timestamp >> 8), static_cast<uint8_t>(timestamp),
            0, 0, 0, ssrc};
        data.insert(data.end(), payload.begin(), payload.end());
        if (hold) { held_ = std::move(data); return; }
        Check(sendto(tx_, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                     reinterpret_cast<sockaddr*>(&destination_), sizeof(destination_)) == static_cast<int>(data.size()),
              "send RTP");
    }

    void FlushHeld() {
        Check(!held_.empty(), "held RTP packet exists");
        Check(sendto(tx_, reinterpret_cast<const char*>(held_.data()), static_cast<int>(held_.size()), 0,
                     reinterpret_cast<sockaddr*>(&destination_), sizeof(destination_)) == static_cast<int>(held_.size()),
              "send reordered RTP packet");
        held_.clear();
    }

    void WaitForRequest() {
        std::unique_lock lock(mutex_);
        Check(changed_.wait_for(lock, std::chrono::seconds(3), [&] { return requests > 0; }),
              "missing-packet deadline must fire even without another arrival");
    }

    void Config(uint32_t timestamp) {
        Send(timestamp, {0x67, 0x42}, false);
        Send(timestamp, {0x68, 0x80}, false);
    }

    void Idr(uint32_t timestamp) {
        Config(timestamp);
        Send(timestamp, {0x65, 0x80}); // first_mb_in_slice = 0
    }

    std::vector<Received> Finish(uint32_t timestamp) {
        const uint64_t us = uint64_t{timestamp} * 1000000 / 90000;
        std::unique_lock lock(mutex_);
        Check(changed_.wait_for(lock, std::chrono::seconds(3), [&] {
            return !frames_.empty() && frames_.back().timestamp == us;
        }), "timed out waiting for final complete frame");
        return frames_;
    }

    uint16_t sequence = 0;
    uint8_t ssrc = 1;
    std::atomic<unsigned> requests{0};

private:
    SOCKET tx_ = INVALID_SOCKET;
    sockaddr_in destination_{};
    std::unique_ptr<RtpVideoTransport> receiver_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Received> frames_;
    std::vector<uint8_t> held_;
};

void CheckRecovery(const std::vector<Received>& frames) {
    Check(frames.size() == 3, "damaged/dependent pictures must be withheld until a complete IDR");
    Check(frames[0].timestamp == 100000, "initial IDR timestamp");
    Check(frames[1].timestamp == 300000 && frames[1].keyframe, "resume at recovery IDR");
    Check(frames[1].discontinuity, "recovery must reset decoder references");
    Check(frames[2].timestamp == 333333 && !frames[2].discontinuity, "normal P frames resume after IDR");
}

void EndRecovery(Stream& stream) {
    stream.Idr(27000);
    stream.Send(30000, {0x41, 0x80});
}

void TestPredictedFramesAfterGap() {
    Stream stream;
    stream.Idr(9000);
    ++stream.sequence;
    stream.Send(12000, {0x41, 0x80});
    stream.Send(15000, {0x41, 0x80});
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
    Check(stream.requests == 1, "request IDR once, without a per-frame request storm");
}

void TestDamagedIdrCannotRecover() {
    Stream stream;
    stream.Idr(9000);
    stream.Config(12000);
    stream.Send(12000, {0x7c, 0x85, 0x80}, false); // first IDR slice, FU start
    ++stream.sequence;                           // middle fragment lost
    stream.Send(12000, {0x7c, 0x45, 0x01}, false);
    stream.Send(12000, {0x65, 0x40});             // second slice is NOT a complete IDR
    stream.Send(15000, {0x41, 0x80});
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
}

void TestMissingMarker() {
    Stream stream;
    stream.Idr(9000);
    stream.Send(12000, {0x41, 0x80}, false);
    stream.Send(15000, {0x41, 0x80}); // contiguous sequence, but old picture was never closed
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
}

void TestFragmentTimestampChange() {
    Stream stream;
    stream.Idr(9000);
    stream.Config(12000);
    stream.Send(12000, {0x7c, 0x85, 0x80}, false);
    stream.Send(15000, {0x7c, 0x45, 0x01}); // must not splice different pictures together
    stream.Send(18000, {0x41, 0x80});
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
}

void TestMalformedStap() {
    Stream stream;
    stream.Idr(9000);
    // Valid IDR followed by a truncated NAL length: do not emit the partial aggregate.
    stream.Send(12000, {0x78, 0, 2, 0x65, 0x80, 0, 9, 0x41});
    stream.Send(15000, {0x41, 0x80});
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
}

void TestOrphanFragment() {
    Stream stream;
    stream.Idr(9000);
    stream.Send(12000, {0x7c, 0x45, 0x01}); // no FU start, even without a sequence gap
    stream.Send(15000, {0x41, 0x80});
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
}

void TestRecoveryNeedsParameterSets() {
    Stream stream;
    stream.Idr(9000);
    ++stream.sequence;
    stream.Send(12000, {0x41, 0x80});
    stream.Send(15000, {0x65, 0x80}); // decoder reset without SPS/PPS cannot recover safely
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
}

void TestJoinMidGop() {
    Stream stream;
    stream.Send(3000, {0x41, 0x80});
    stream.Send(6000, {0x65, 0x40}); // tail IDR slice is not a clean join point
    stream.Idr(9000);
    stream.Send(12000, {0x41, 0x80});
    const auto frames = stream.Finish(12000);
    Check(frames.size() == 2 && frames[0].keyframe && frames[0].discontinuity,
          "joining mid-GOP waits for a self-contained IDR");
}

void TestDuplicateAndSequenceWrap() {
    Stream stream;
    stream.sequence = 65532;
    stream.Idr(9000); // last packet sequence 65535
    --stream.sequence;
    stream.Send(9000, {0x65, 0x80}); // duplicate last packet
    stream.Send(12000, {0x41, 0x80}); // sequence wraps to zero
    const auto frames = stream.Finish(12000);
    Check(frames.size() == 2 && !frames[1].discontinuity && stream.requests == 0,
          "duplicates and sequence wrap do not trigger recovery");
}

void TestFragmentedAndMultiSliceIdr() {
    Stream stream;
    stream.Config(9000);
    stream.Send(9000, {0x7c, 0x85, 0x80, 0x11}, false);
    stream.Send(9000, {0x7c, 0x05, 0x22}, false);
    stream.Send(9000, {0x7c, 0x45, 0x33}, false);
    stream.Send(9000, {0x65, 0x40}); // second complete IDR slice
    stream.Send(12000, {0x41, 0x80});
    const auto frames = stream.Finish(12000);
    const std::vector<uint8_t> expected = {0,0,0,1,0x67,0x42, 0,0,0,1,0x68,0x80,
        0,0,0,1,0x65,0x80,0x11,0x22,0x33, 0,0,0,1,0x65,0x40};
    Check(frames.size() == 2 && frames[0].payload == expected && stream.requests == 0,
          "healthy fragmented/multi-slice IDR must reassemble byte-for-byte");
}

void TestValidStap() {
    Stream stream;
    stream.Send(9000, {0x78, 0,2,0x67,0x42, 0,2,0x68,0x80, 0,2,0x65,0x80});
    stream.Send(12000, {0x41, 0x80});
    const auto frames = stream.Finish(12000);
    Check(frames.size() == 2 && frames[0].timestamp == 100000 && frames[0].keyframe,
          "valid STAP-A carrying SPS/PPS/IDR uses the picture clock");
}

void TestFragmentMarkerWithoutEnd() {
    Stream stream;
    stream.Idr(9000);
    stream.Config(12000);
    stream.Send(12000, {0x7c, 0x85, 0x80}, true);
    stream.Send(15000, {0x41, 0x80});
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
}

void TestFragmentHeaderMismatch() {
    Stream stream;
    stream.Idr(9000);
    stream.Config(12000);
    stream.Send(12000, {0x7c, 0x85, 0x80}, false);
    stream.Send(12000, {0x7c, 0x41, 0x01}); // switches from IDR to non-IDR mid-NAL
    stream.Send(15000, {0x41, 0x80});
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
}

void TestNalInterruptsFragment() {
    Stream stream;
    stream.Idr(9000);
    stream.Config(12000);
    stream.Send(12000, {0x7c, 0x85, 0x80}, false);
    stream.Send(12000, {0x65, 0x40});
    stream.Send(15000, {0x41, 0x80});
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
}

void TestNewSessionMustRecover() {
    Stream stream;
    stream.Idr(9000);
    stream.ssrc = 2;
    stream.Send(12000, {0x41, 0x80});
    EndRecovery(stream);
    CheckRecovery(stream.Finish(30000));
}

void TestReorderedFragmentDoesNotFreeze() {
    Stream stream;
    stream.Config(9000);
    stream.Send(9000, {0x7c, 0x85, 0x80}, false);
    stream.Send(9000, {0x7c, 0x05, 0x11}, false, true);
    stream.Send(9000, {0x7c, 0x45, 0x22});
    stream.FlushHeld();
    stream.Send(12000, {0x41, 0x80});
    const auto frames = stream.Finish(12000);
    Check(frames.size() == 2 && stream.requests == 0 && !frames[1].discontinuity,
          "slightly reordered FU must not discard clean pictures or trigger IDR recovery");
}

void TestMissingPacketDeadlineOnQuietSocket() {
    Stream stream;
    stream.Idr(9000);
    stream.Finish(9000);
    ++stream.sequence;
    stream.Send(12000, {0x41, 0x80});
    stream.WaitForRequest(); // no subsequent network packet to wake recvfrom
}
} // namespace

int main() {
    unsigned failures = 0;
    const struct { const char* name; void (*test)(); } tests[] = {
        {"predicted frames after gap", TestPredictedFramesAfterGap},
        {"damaged multi-slice IDR", TestDamagedIdrCannotRecover},
        {"missing marker", TestMissingMarker},
        {"FU timestamp change", TestFragmentTimestampChange},
        {"malformed STAP", TestMalformedStap},
        {"orphan FU continuation", TestOrphanFragment},
        {"recovery parameter sets", TestRecoveryNeedsParameterSets},
        {"join mid-GOP", TestJoinMidGop},
        {"duplicates and wrap", TestDuplicateAndSequenceWrap},
        {"healthy fragmented multi-slice IDR", TestFragmentedAndMultiSliceIdr},
        {"healthy STAP-A", TestValidStap},
        {"marker without FU end", TestFragmentMarkerWithoutEnd},
        {"FU header mismatch", TestFragmentHeaderMismatch},
        {"NAL interrupts FU", TestNalInterruptsFragment},
        {"new SSRC requires recovery", TestNewSessionMustRecover},
        {"reordered FU does not freeze", TestReorderedFragmentDoesNotFreeze},
        {"quiet socket expires missing packet", TestMissingPacketDeadlineOnQuietSocket},
    };
    for (const auto& test : tests) {
        try {
            test.test();
            std::cout << "PASS: " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures ? 1 : 0;
}
