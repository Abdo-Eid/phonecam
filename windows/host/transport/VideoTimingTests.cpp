#include <winsock2.h>
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "transport/RtpVideoTransport.h"
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

void TestRtp() {
    using namespace phonecam::transport;
    WSADATA wsa;
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Winsock");
    const SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Obtain an unused local port without touching the live PhoneCam port.
    Check(bind(sender, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0, "test bind");
    int length = sizeof(destination);
    getsockname(sender, reinterpret_cast<sockaddr*>(&destination), &length);
    const uint16_t port = ntohs(destination.sin_port);
    closesocket(sender);
    const SOCKET tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    RtpVideoTransport receiver(port);
    Check(receiver.Connect(), "receiver bind");
    struct Received { PacketType type; uint64_t timestamp; bool discontinuity; };
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Received> packets;
    std::thread worker([&] {
        receiver.RunReceiveLoop([&](const VideoPacket& packet) {
            std::lock_guard lock(mutex);
            packets.push_back({packet.type, packet.timestampUs, packet.discontinuity});
            changed.notify_all();
        });
    });
    uint16_t sequence = 0;
    auto send = [&](uint32_t ssrc, uint32_t timestamp, uint8_t nal, bool marker = true) {
        const uint16_t seq = ++sequence;
        const uint8_t data[] = {0x80, static_cast<uint8_t>(96 | (marker ? 0x80 : 0)),
            static_cast<uint8_t>(seq >> 8), static_cast<uint8_t>(seq),
            static_cast<uint8_t>(timestamp >> 24), static_cast<uint8_t>(timestamp >> 16),
            static_cast<uint8_t>(timestamp >> 8), static_cast<uint8_t>(timestamp),
            static_cast<uint8_t>(ssrc >> 24), static_cast<uint8_t>(ssrc >> 16),
            static_cast<uint8_t>(ssrc >> 8), static_cast<uint8_t>(ssrc), nal, 0x80};
        sendto(tx, reinterpret_cast<const char*>(data), sizeof(data), 0,
               reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    };
    send(1, 0, 0x67);                  // MediaCodec's independent config clock
    send(1, 0xe0000000, 0x65);         // used to be rejected after PTS=0
    send(1, 0xe0000bb8, 0x41);
    send(2, 0xfffffff0, 0x65);         // a new encoder session resets the decoder
    send(2, 0x00000100, 0x41);         // natural 32-bit RTP wrap
    send(2, 0x00001000, 0x41, false);  // lost marker: do not emit this picture
    send(2, 0x00002000, 0x65);
    bool complete;
    {
        std::unique_lock lock(mutex);
        complete = changed.wait_for(lock, std::chrono::seconds(3), [&] { return packets.size() >= 6; });
    }
    receiver.Disconnect();
    worker.join();
    closesocket(tx);
    WSACleanup();
    Check(complete && packets.size() == 6, "all complete pictures delivered; truncated picture discarded");
    Check(packets[0].type == PacketType::Config && packets[0].discontinuity, "config starts session");
    Check(packets[1].timestamp == uint64_t{0xe0000000} * 1000000 / 90000, "real camera PTS after config");
    Check(packets[2].timestamp > packets[1].timestamp, "frame time increases");
    Check(packets[3].discontinuity, "new SSRC marks decoder reset");
    Check(packets[4].timestamp == ((uint64_t{1} << 32) + 0x100) * 1000000 / 90000, "RTP wrap extends clock");
    Check(packets[5].timestamp > packets[4].timestamp, "correct timestamp after missing marker");
}
}

int main() {
    try {
        TestClock();
        TestRtp();
        std::cout << "Video pacing and RTP startup tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
