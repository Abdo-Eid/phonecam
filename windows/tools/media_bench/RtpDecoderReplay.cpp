#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "RtpDecoderReplay.h"
#include "decode/MFH264Decoder.h"
#include "transport/RtpVideoTransport.h"

namespace {
using Bytes = std::vector<uint8_t>;
using Pictures = std::map<uint64_t, uint64_t>;
using namespace phonecam::transport;

struct Nal { size_t begin, end; };
std::vector<Nal> Nals(const Bytes& bytes) {
    std::vector<Nal> result;
    for (size_t i = 0; i + 3 < bytes.size();) {
        size_t prefix = 0;
        if (bytes[i] == 0 && bytes[i + 1] == 0) {
            if (bytes[i + 2] == 1) prefix = 3;
            else if (i + 4 < bytes.size() && bytes[i + 2] == 0 && bytes[i + 3] == 1) prefix = 4;
        }
        if (!prefix) { ++i; continue; }
        if (!result.empty()) result.back().end = i;
        result.push_back({i + prefix, bytes.size()});
        i += prefix + 1;
    }
    return result;
}

class Replay {
public:
    Replay() {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("Winsock startup failed");
        tx_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        destination_.sin_family = AF_INET;
        destination_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(tx_, reinterpret_cast<sockaddr*>(&destination_), sizeof(destination_)) != 0)
            throw std::runtime_error("Loopback port reservation failed");
        int size = sizeof(destination_);
        if (getsockname(tx_, reinterpret_cast<sockaddr*>(&destination_), &size) != 0)
            throw std::runtime_error("Loopback port lookup failed");
        const uint16_t port = ntohs(destination_.sin_port);
        closesocket(tx_);
        tx_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        receiver_ = std::make_unique<RtpVideoTransport>(port);
        if (!receiver_->Connect()) throw std::runtime_error("Loopback RTP receiver bind failed");
    }

    ~Replay() {
        receiver_->Disconnect();
        if (worker_.joinable()) worker_.join();
        closesocket(tx_);
        receiver_.reset();
        WSACleanup();
    }

    Pictures Run(const std::vector<Bytes>& units, int firstUnit, int droppedFrame, bool dropSps, bool reorderOne) {
        const uint64_t finalPts = (units.size() - 1) * 1000000ULL / 30;
        worker_ = std::thread([&, finalPts] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                if (FAILED(apartment)) throw std::runtime_error("Decoder thread COM startup failed");
                phonecam::decode::MFH264Decoder decoder;
                if (!decoder.Initialize()) throw std::runtime_error("Decoder initialization failed");
                receiver_->RunReceiveLoop([&](const VideoPacket& packet) {
                    if (packet.discontinuity && !decoder.Initialize()) throw std::runtime_error("Decoder reset failed");
                    if (!decoder.Feed(packet.payload, packet.payloadSize, packet.timestampUs,
                        [&](const phonecam::decode::DecodedFrame& frame) {
                            uint64_t hash = 14695981039346656037ULL;
                            for (size_t i = 0; i < frame.nv12Size; ++i) hash = (hash ^ frame.nv12[i]) * 1099511628211ULL;
                            pictures_[frame.timestampUs] = hash;
                        })) throw std::runtime_error("Decoder rejected a delivered RTP access unit");
                    if (packet.type == PacketType::Frame && packet.timestampUs == finalPts) {
                        std::lock_guard lock(mutex_);
                        complete_ = true;
                        changed_.notify_all();
                    }
                });
            } catch (const std::exception& error) {
                std::lock_guard lock(mutex_);
                failure_ = error.what();
                changed_.notify_all();
            }
            if (SUCCEEDED(apartment)) CoUninitialize();
        });

        for (int frame = firstUnit; frame < static_cast<int>(units.size()); ++frame) {
            const auto& bytes = units[frame];
            const auto nals = Nals(bytes);
            for (size_t n = 0; n < nals.size(); ++n) {
                const auto& nal = nals[n];
                const uint8_t header = bytes[nal.begin];
                const uint8_t type = header & 31;
                const bool vcl = type >= 1 && type <= 5;
                const size_t length = nal.end - nal.begin;
                const bool lastNal = n + 1 == nals.size();
                const uint32_t timestamp = frame * 3000;
                if (length <= 256) {
                    const bool drop = !dropped_ && frame == droppedFrame && (dropSps ? type == 7 : vcl);
                    Send(timestamp, lastNal, Bytes(bytes.begin() + nal.begin, bytes.begin() + nal.end), drop);
                } else {
                    unsigned fragment = 0;
                    for (size_t offset = nal.begin + 1; offset < nal.end; ++fragment) {
                        const size_t count = (std::min)(size_t{254}, nal.end - offset);
                        const bool start = offset == nal.begin + 1;
                        const bool end = offset + count == nal.end;
                        Bytes payload{static_cast<uint8_t>((header & 0xe0) | 28),
                                      static_cast<uint8_t>(type | (start ? 0x80 : 0) | (end ? 0x40 : 0))};
                        payload.insert(payload.end(), bytes.begin() + offset, bytes.begin() + offset + count);
                        const bool drop = !dropped_ && frame == droppedFrame && !dropSps && vcl && fragment == 1;
                        const bool hold = reorderOne && !reordered_ && frame == 45 && vcl && fragment == 1;
                        Send(timestamp, lastNal && end, payload, drop, hold);
                        offset += count;
                    }
                }
            }
            // Synthetic 30 fps replay, not a throughput test. Avoid flooding
            // the loopback socket faster than an HD software decoder can drain.
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        if (droppedFrame >= 0 && !dropped_) throw std::runtime_error("Requested synthetic loss was not injected");
        if (reorderOne && !reordered_) throw std::runtime_error("Requested reordering was not injected");
        {
            std::unique_lock lock(mutex_);
            if (!changed_.wait_for(lock, std::chrono::seconds(15), [&] { return complete_ || !failure_.empty(); }))
                throw std::runtime_error("RTP replay timed out before final frame");
            if (!failure_.empty()) throw std::runtime_error(failure_);
        }
        receiver_->Disconnect();
        worker_.join();
        return pictures_;
    }

private:
    void Send(uint32_t timestamp, bool marker, const Bytes& payload, bool drop, bool hold = false) {
        const uint16_t seq = ++sequence_;
        if (drop) { dropped_ = true; return; }
        Bytes data{0x80, static_cast<uint8_t>(96 | (marker ? 0x80 : 0)),
                   static_cast<uint8_t>(seq >> 8), static_cast<uint8_t>(seq),
                   static_cast<uint8_t>(timestamp >> 24), static_cast<uint8_t>(timestamp >> 16),
                   static_cast<uint8_t>(timestamp >> 8), static_cast<uint8_t>(timestamp), 0,0,0,1};
        data.insert(data.end(), payload.begin(), payload.end());
        if (hold) {
            held_ = std::move(data);
            reordered_ = true;
            return;
        }
        if (sendto(tx_, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                   reinterpret_cast<sockaddr*>(&destination_), sizeof(destination_)) != static_cast<int>(data.size()))
            throw std::runtime_error("RTP replay send failed");
        if (!held_.empty()) {
            if (sendto(tx_, reinterpret_cast<const char*>(held_.data()), static_cast<int>(held_.size()), 0,
                       reinterpret_cast<sockaddr*>(&destination_), sizeof(destination_)) != static_cast<int>(held_.size()))
                throw std::runtime_error("Reordered RTP replay send failed");
            held_.clear();
        }
    }

    SOCKET tx_ = INVALID_SOCKET;
    sockaddr_in destination_{};
    std::unique_ptr<RtpVideoTransport> receiver_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool complete_ = false;
    bool dropped_ = false;
    bool reordered_ = false;
    Bytes held_;
    uint16_t sequence_ = 0;
    std::string failure_;
    Pictures pictures_;
};
} // namespace

void VerifyRtpRecovery(const std::vector<Bytes>& units, const Pictures& reference,
                       const std::filesystem::path& output) {
    if (units.size() < 120) throw std::runtime_error("RTP recovery fixture needs 120 frames, GOP 30, repeated SPS/PPS");
    struct Scenario { const char* name; int first, lost, resume; bool sps; bool reorder = false; };
    const Scenario scenarios[] = {
        {"rtp-clean", 0, -1, 0, false},
        {"rtp-p-fragment-loss", 0, 45, 60, false},
        {"rtp-idr-fragment-loss", 0, 60, 90, false},
        {"rtp-idr-sps-loss", 0, 60, 90, true},
        {"rtp-join-mid-gop", 45, -1, 60, false},
        {"rtp-one-fragment-reordered", 0, -1, 0, false, true},
    };
    std::ofstream summary(output / "rtp-summary.csv");
    summary << "case,reference_frames,delivered_frames,hash_mismatches,recovery_frame\n";
    for (const auto& scenario : scenarios) {
        Replay replay;
        const auto pictures = replay.Run(units, scenario.first, scenario.lost, scenario.sps, scenario.reorder);
        unsigned mismatches = 0;
        for (const auto& [pts, hash] : pictures) {
            const auto expected = reference.find(pts);
            if (expected == reference.end() || expected->second != hash) ++mismatches;
        }
        summary << scenario.name << ',' << reference.size() << ',' << pictures.size() << ',' << mismatches << ','
                << scenario.resume << '\n';
        summary.flush();
        if (mismatches) throw std::runtime_error(std::string(scenario.name) + ": corrupted decoded pictures delivered");
        if (scenario.lost < 0 && scenario.first == 0 && pictures != reference)
            throw std::runtime_error("Healthy RTP replay differs from direct decoder reference");
        const int withheldFrom = scenario.lost >= 0 ? scenario.lost : scenario.first;
        if (scenario.resume > withheldFrom) {
            const auto first = pictures.lower_bound(withheldFrom * 1000000ULL / 30);
            if (first != pictures.end() && first->first < scenario.resume * 1000000ULL / 30)
                throw std::runtime_error(std::string(scenario.name) + ": unsafe dependent pictures delivered");
        }
        for (const auto& [pts, hash] : reference) {
            if (pts < scenario.resume * 1000000ULL / 30) continue;
            const auto found = pictures.find(pts);
            if (found == pictures.end() || found->second != hash)
                throw std::runtime_error(std::string(scenario.name) + ": recovery did not match reference");
        }
    }
    std::cout << "RTP/MF image-hash recovery: 6 scenarios passed; see rtp-summary.csv\n";
}
