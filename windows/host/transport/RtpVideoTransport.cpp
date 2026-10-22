#include "transport/RtpVideoTransport.h"
#include "transport/RtpReorderBuffer.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log/Log.h"

#pragma comment(lib, "ws2_32.lib")

namespace phonecam::transport {

namespace {

constexpr uint8_t kRtpVersion = 2;
constexpr uint8_t kPayloadTypeH264 = 96;  // dynamic, matches the phone's packetizer
constexpr size_t kRtpHeaderSize = 12;
constexpr size_t kMaxDatagram = 2048;

// NAL unit types (ITU-T H.264 table 7-1) this receiver understands.
constexpr uint8_t kNalSlice = 1;
constexpr uint8_t kNalIdr = 5;
constexpr uint8_t kNalStapA = 24;
constexpr uint8_t kNalFuA = 28;

// Reads a ue(v) Exp-Golomb value (H.264 9.1) from the RBSP starting at
// [offset]. Used for first_mb_in_slice, which is how a new picture is
// recognized inside a continuous NAL stream -- the RTP marker bit alone
// isn't enough, because a lost final packet of an access unit means that
// marker never arrives.
bool ReadExpGolomb(const uint8_t* data, size_t size, size_t offset, uint32_t& out) {
    if (offset >= size) return false;
    size_t bit = 0;
    int leadingZeros = 0;
    while (offset + (bit / 8) < size) {
        const uint8_t byte = data[offset + bit / 8];
        const int bitValue = (byte >> (7 - (bit % 8))) & 1;
        if (bitValue) break;
        ++leadingZeros;
        ++bit;
    }
    if (leadingZeros > 31) return false;  // no terminator / absurd value
    if (offset + (bit / 8) >= size) return false;

    ++bit;  // consume the terminating '1'
    uint32_t value = 0;
    for (int i = 0; i < leadingZeros; ++i) {
        if (offset + (bit / 8) >= size) return false;
        const uint8_t byte = data[offset + bit / 8];
        value = (value << 1) | ((byte >> (7 - (bit % 8))) & 1);
        ++bit;
    }
    out = (1u << leadingZeros) - 1u + value;
    return true;
}

bool IsVclType(uint8_t nalType) { return nalType >= 1 && nalType <= 5; }

// One reassembled access unit: Annex-B bytes (4-byte start code before every
// NAL), ready to hand to the decoder.
struct AccessUnit {
    std::vector<uint8_t> data;
    bool hasVcl = false;
    bool hasIdr = false;
    bool hasFirstIdrSlice = false;
    bool hasSps = false;
    bool hasPps = false;
    uint64_t timestampUs = 0;

    void clear() {
        data.clear();
        hasVcl = false;
        hasIdr = false;
        hasFirstIdrSlice = false;
        hasSps = false;
        hasPps = false;
    }
};

}  // namespace

struct RtpVideoTransport::Impl {
    uint16_t listenPort = kDefaultRtpPort;
    SOCKET sock = INVALID_SOCKET;
    bool wsaOk = false;

    // Guards sock and lets a Disconnect() arriving mid-Connect() win, same
    // pattern as NetVideoTransport/AdbVideoTransport.
    std::mutex mutex;
    std::atomic<bool> cancelled{false};

    std::function<void()> keyframeRequester;

    // Reassembly state.
    AccessUnit unit;
    std::vector<uint8_t> fuBuffer;   // FU-A payload being reassembled
    bool fuActive = false;
    uint8_t fuNalType = 0;
    uint8_t fuIndicator = 0;

    bool haveSeq = false;
    uint16_t lastSeq = 0;
    bool haveSsrc = false;
    bool newSession = false;
    uint32_t ssrc = 0;
    uint64_t tsBase = 0;   // unwrapped 32-bit RTP timestamp high part
    uint32_t lastTs = 0;
    bool haveTs = false;

    // A NAL boundary is NOT a decoder recovery point: P pictures can still
    // reference a lost picture, and another IDR slice can belong to the same
    // damaged access unit. Resume only at a complete, self-contained IDR.
    bool waitingForIdr = true;
    bool haveDiscardedTimestamp = false;
    uint32_t discardedTimestamp = 0;
    uint32_t lostPackets = 0;
    std::chrono::steady_clock::time_point lastKeyframeRequest{};

    // Rolling counters, logged periodically: on a wireless link the difference between
    // "no video" and "video that stopped arriving" is invisible without them.
    uint64_t datagrams = 0;
    uint64_t bytes = 0;
    uint64_t accessUnits = 0;
    uint64_t recoveryDrops = 0;
    std::chrono::steady_clock::time_point lastStatsLog{};

    void Emit(const AccessUnit& finished, uint32_t& sequence, const VideoPacketCallback& onPacket) {
        if (finished.data.empty()) return;
        if (finished.hasVcl && waitingForIdr) {
            // The project's sender repeats SPS/PPS inside every IDR AU. Do not
            // restart a reset decoder using stale/missing parameter sets, or
            // mistake a surviving non-first IDR slice for an intact picture.
            if (!finished.hasIdr || !finished.hasFirstIdrSlice || !finished.hasSps || !finished.hasPps) {
                ++recoveryDrops;
                RequestKeyframe();
                return;
            }
            waitingForIdr = false;
            newSession = true; // the bridge must discard old decoder references
            phonecam::log::Info("RtpVideoTransport: resuming at a complete IDR with SPS/PPS");
        }
        ++accessUnits;
        VideoPacket packet{finished.hasVcl ? PacketType::Frame : PacketType::Config,
                           finished.hasIdr, sequence++, finished.timestampUs,
                           finished.data.data(), finished.data.size(), newSession};
        newSession = false;
        onPacket(packet);
    }

    bool AppendNal(const uint8_t* nal, size_t size, uint8_t nalType) {
        uint32_t firstMb = 0;
        if (IsVclType(nalType)) {
            if (!ReadExpGolomb(nal, size, 1, firstMb)) return false;
            if (unit.hasVcl && (firstMb == 0 || (nalType == kNalIdr) != unit.hasIdr)) return false;
        }
        static const uint8_t kStartCode[4] = {0, 0, 0, 1};
        unit.data.insert(unit.data.end(), kStartCode, kStartCode + 4);
        unit.data.insert(unit.data.end(), nal, nal + size);
        if (IsVclType(nalType)) unit.hasVcl = true;
        if (nalType == kNalIdr) unit.hasIdr = true;
        if (nalType == 7) unit.hasSps = true;
        if (nalType == 8) unit.hasPps = true;
        if (nalType == kNalIdr && firstMb == 0) unit.hasFirstIdrSlice = true;
        return true;
    }

    // Drops the in-progress access unit (a loss made its contents
    // unreconstructable) and asks for a keyframe so the decoder can recover
    // promptly instead of waiting for the encoder's next periodic IDR.
    void OnLoss(const char* reason) {
        unit.clear();
        fuActive = false;
        fuBuffer.clear();
        waitingForIdr = true;
        newSession = true;
        ++lostPackets;
        phonecam::log::Info(std::string("RtpVideoTransport: waiting for clean IDR after ") + reason);
        RequestKeyframe();
    }

    void DiscardTimestamp(uint32_t timestamp, const char* reason) {
        OnLoss(reason);
        // Discard the rest of this damaged picture, including any surviving
        // IDR slices. Recovery is allowed only at a later RTP timestamp.
        haveDiscardedTimestamp = true;
        discardedTimestamp = timestamp;
    }

    void RequestKeyframe() {
        const auto now = std::chrono::steady_clock::now();
        if (keyframeRequester && (lastKeyframeRequest.time_since_epoch().count() == 0 ||
                                 now - lastKeyframeRequest > std::chrono::seconds(1))) {
            lastKeyframeRequest = now;
            phonecam::log::Info("RtpVideoTransport: requesting a recovery keyframe");
            keyframeRequester();
        }
    }
};

RtpVideoTransport::RtpVideoTransport(uint16_t listenPort) : impl_(std::make_unique<Impl>()) {
    impl_->listenPort = listenPort;
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        phonecam::log::Error("RtpVideoTransport: WSAStartup failed");
        return;
    }
    impl_->wsaOk = true;
}

RtpVideoTransport::~RtpVideoTransport() {
    Disconnect();
    if (impl_->wsaOk) {
        WSACleanup();
    }
}

void RtpVideoTransport::SetKeyframeRequester(std::function<void()> requester) {
    impl_->keyframeRequester = std::move(requester);
}

bool RtpVideoTransport::Connect() {
    if (!impl_->wsaOk) return false;
    impl_->cancelled.store(false);

    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        phonecam::log::Error("RtpVideoTransport: socket() failed");
        return false;
    }

    // A large receive buffer absorbs a keyframe burst (a 1080p IDR is ~100 KB
    // spread over ~70 datagrams) without the kernel dropping any of it --
    // dropped datagrams here are indistinguishable from Wi-Fi loss, and
    // would trigger a pointless keyframe request.
    int rcvBuf = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(impl_->listenPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        phonecam::log::Error("RtpVideoTransport: bind() failed -- is another host already using the port?");
        closesocket(s);
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->cancelled.load()) {
        closesocket(s);
        return false;
    }
    impl_->sock = s;
    impl_->haveSsrc = false;
    phonecam::log::Info("RtpVideoTransport: listening on UDP port " + std::to_string(impl_->listenPort));
    return true;
}

void RtpVideoTransport::Disconnect() {
    impl_->cancelled.store(true);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sock != INVALID_SOCKET) {
        // shutdown() + closesocket() is the documented-safe way to unblock a
        // recvfrom() already in flight on this socket from the receive thread.
        shutdown(impl_->sock, SD_BOTH);
        closesocket(impl_->sock);
        impl_->sock = INVALID_SOCKET;
    }
}

void RtpVideoTransport::RunReceiveLoop(const VideoPacketCallback& onPacket) {
    std::vector<uint8_t> incoming(kMaxDatagram);
    RtpReorderBuffer reorder;
    uint32_t emittedSeq = 0;
    SOCKET sock;
    {
        std::lock_guard lock(impl_->mutex);
        sock = impl_->sock;
    }

    auto receiveOrdered = [&]() -> std::optional<std::vector<uint8_t>> {
        while (!impl_->cancelled.load()) {
            const auto now = RtpReorderBuffer::Clock::now();
            if (auto packet = reorder.Pop(now)) return packet;
            if (const auto deadline = reorder.Deadline()) {
                // Wake even if the missing packet NEVER arrives. Otherwise a
                // quiet socket would turn a bounded reorder window into a stall.
                const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(*deadline - now);
                timeval timeout{0, static_cast<long>((std::max)(int64_t{1}, remaining.count()))};
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(sock, &readable);
                const int ready = select(0, &readable, nullptr, nullptr, &timeout);
                if (ready == 0) continue;
                if (ready == SOCKET_ERROR) return std::nullopt;
            }
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int received = recvfrom(sock, reinterpret_cast<char*>(incoming.data()),
                static_cast<int>(incoming.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (received <= 0) {
                if (impl_->cancelled.load()) return std::nullopt;
                const int error = WSAGetLastError();
                if (error == WSAECONNRESET || error == WSAEWOULDBLOCK) {
                    if (reorder.Pending() == 0) Sleep(50);
                    continue;
                }
                return std::nullopt;
            }
            if (received >= static_cast<int>(kRtpHeaderSize)) {
                ++impl_->datagrams;
                impl_->bytes += received;
            }
            reorder.Push(incoming.data(), static_cast<size_t>(received), RtpReorderBuffer::Clock::now());
        }
        return std::nullopt;
    };

    while (true) {
        const auto datagram = receiveOrdered();
        if (!datagram) break;
        const size_t size = datagram->size();
        const uint8_t* data = datagram->data();
        {
            const auto now = std::chrono::steady_clock::now();
            if (impl_->lastStatsLog.time_since_epoch().count() == 0) {
                impl_->lastStatsLog = now;
            } else if (now - impl_->lastStatsLog >= std::chrono::seconds(5)) {
                impl_->lastStatsLog = now;
                phonecam::log::Info("RtpVideoTransport: " + std::to_string(impl_->datagrams) +
                                     " datagrams, " + std::to_string(impl_->bytes / 1024) + " KiB, " +
                                     std::to_string(impl_->accessUnits) + " access units, " +
                                     std::to_string(impl_->lostPackets) + " loss events, " +
                                     std::to_string(impl_->recoveryDrops) + " pictures withheld for recovery, " +
                                     std::to_string(reorder.latePacketsRecovered) + " late packets recovered, " +
                                     std::to_string(reorder.expiredGaps) + " reorder gaps expired, " +
                                     std::to_string(reorder.lateOrDuplicate) + " late/duplicate packets ignored, " +
                                     std::to_string(reorder.capacityDrops) + " reorder capacity drops");
            }
        }

        const uint8_t version = (data[0] >> 6) & 0x03;
        if (version != kRtpVersion) continue;
        const bool padding = (data[0] & 0x20) != 0;
        const bool extension = (data[0] & 0x10) != 0;
        const uint8_t csrcCount = data[0] & 0x0F;
        const bool marker = (data[1] & 0x80) != 0;
        const uint8_t payloadType = data[1] & 0x7F;
        const uint16_t seq = static_cast<uint16_t>((data[2] << 8) | data[3]);
        const uint32_t timestamp = (static_cast<uint32_t>(data[4]) << 24) |
                                  (static_cast<uint32_t>(data[5]) << 16) |
                                  (static_cast<uint32_t>(data[6]) << 8) | static_cast<uint32_t>(data[7]);
        const uint32_t ssrc = (static_cast<uint32_t>(data[8]) << 24) | (static_cast<uint32_t>(data[9]) << 16) |
                              (static_cast<uint32_t>(data[10]) << 8) | static_cast<uint32_t>(data[11]);

        size_t offset = kRtpHeaderSize + static_cast<size_t>(csrcCount) * 4;
        if (extension) {
            if (offset + 4 > size) continue;
            const size_t extWords = (static_cast<size_t>(data[offset + 2]) << 8) | data[offset + 3];
            offset += 4 + extWords * 4;
        }
        if (offset > size) continue;
        size_t payloadSize = size - offset;
        if (padding) {
            if (payloadSize == 0) continue;
            payloadSize -= data[size - 1];  // padding count
            if (payloadSize > size - offset) continue;
        }
        const uint8_t* payload = data + offset;
        if (payloadSize == 0) continue;
        if (payloadType != kPayloadTypeH264) {
            // Only H.264 is negotiated; anything else is ignored rather than
            // treated as a framing error.
            continue;
        }

        // A new SSRC means a new sender session (the phone restarted, or a
        // new phone): reset every continuity assumption.
        if (!impl_->haveSsrc || impl_->ssrc != ssrc) {
            impl_->haveSsrc = true;
            impl_->ssrc = ssrc;
            impl_->newSession = true;
            impl_->haveSeq = false;
            impl_->haveTs = false;
            impl_->tsBase = 0;
            impl_->waitingForIdr = true;
            impl_->haveDiscardedTimestamp = false;
            impl_->lastKeyframeRequest = {};
            impl_->unit.clear();
            impl_->fuActive = false;
            impl_->fuBuffer.clear();
        }

        // Sequence continuity (RFC 3550's rules): a FORWARD jump means packets were
        // lost, so whatever is half-assembled can no longer be trusted. A BACKWARD
        // jump is a reordered (or duplicate) packet -- a real thing on Wi-Fi -- and
        // must be ignored rather than counted as a loss: reporting it as one would
        // discard a perfectly good access unit and ask for a keyframe for nothing.
        if (impl_->haveSeq) {
            const uint16_t expected = static_cast<uint16_t>(impl_->lastSeq + 1);
            if (seq != expected) {
                const uint16_t forward = static_cast<uint16_t>(seq - expected);
                if (forward < 0x8000u) {
                    impl_->DiscardTimestamp(timestamp, "sequence gap");
                    phonecam::log::Error("RtpVideoTransport: sequence gap, " + std::to_string(forward) +
                                         " packet(s) lost");
                } else {
                    continue;  // reordered or duplicate: ignore it
                }
            }
        }
        impl_->haveSeq = true;
        impl_->lastSeq = seq;

        if (impl_->haveDiscardedTimestamp) {
            if (timestamp == impl_->discardedTimestamp) {
                impl_->RequestKeyframe();
                continue;
            }
            impl_->haveDiscardedTimestamp = false;
        }

        const uint8_t nalType = payload[0] & 0x1F;
        if ((payload[0] & 0x80) != 0) {
            impl_->DiscardTimestamp(timestamp, "forbidden NAL bit");
            continue;
        }
        bool hasVideoClock = IsVclType(nalType) ||
            (nalType == kNalFuA && payloadSize >= 2 && IsVclType(payload[1] & 0x1F));
        if (nalType == kNalStapA) {
            // Validate the entire aggregation BEFORE appending any NAL. An
            // incomplete tail must never turn the intact prefix into a frame.
            size_t pos = 1;
            bool valid = payloadSize > 1;
            while (valid && pos < payloadSize) {
                if (pos + 2 > payloadSize) { valid = false; break; }
                const size_t nalSize = (static_cast<size_t>(payload[pos]) << 8) | payload[pos + 1];
                pos += 2;
                if (nalSize == 0 || nalSize > payloadSize - pos) { valid = false; break; }
                const uint8_t type = payload[pos] & 0x1F;
                if ((payload[pos] & 0x80) != 0 || type == 0 || type >= kNalStapA) { valid = false; break; }
                hasVideoClock = hasVideoClock || IsVclType(type);
                pos += nalSize;
            }
            if (!valid) {
                impl_->DiscardTimestamp(timestamp, "malformed STAP-A");
                continue;
            }
        }
        // Config-only buffers can have timestamp zero, unrelated to the camera
        // clock. Do not use those to reject or unwrap real picture timestamps.
        // Packet sequence numbers above already reject reordered datagrams.
        if (hasVideoClock && !impl_->haveTs) {
            impl_->haveTs = true;
            impl_->lastTs = timestamp;
            impl_->tsBase = 0;
        } else if (hasVideoClock) {
            if (timestamp < impl_->lastTs && impl_->lastTs - timestamp > 0x80000000u) {
                impl_->tsBase += (1ull << 32);
            }
            impl_->lastTs = timestamp;
        }
        const uint64_t ticks = impl_->tsBase + timestamp;
        const uint64_t timestampUs = ticks * 1000000ull / 90000ull;
        if ((impl_->unit.hasVcl || impl_->fuActive) && impl_->unit.timestampUs != timestampUs) {
            // The previous marker was lost. Never stamp the old picture with
            // the new picture's time or deliver a potentially truncated frame.
            impl_->OnLoss("missing marker");
        }
        if (hasVideoClock || !impl_->unit.hasVcl) impl_->unit.timestampUs = timestampUs;

        // ---- payload handling ------------------------------------------------

        if (nalType == kNalFuA) {
            if (payloadSize < 3) {
                impl_->DiscardTimestamp(timestamp, "empty/truncated FU-A");
                continue;
            }
            const uint8_t fuHeader = payload[1];
            const bool start = (fuHeader & 0x80) != 0;
            const bool end = (fuHeader & 0x40) != 0;
            const uint8_t type = fuHeader & 0x1F;
            const uint8_t* fragment = payload + 2;
            const size_t fragmentSize = payloadSize - 2;

            if ((fuHeader & 0x20) != 0 || (start && end) || type == 0 || type >= kNalStapA || (marker && !end)) {
                impl_->DiscardTimestamp(timestamp, "invalid FU-A header/marker");
                continue;
            }

            if (start) {
                if (impl_->fuActive) {
                    impl_->DiscardTimestamp(timestamp, "FU-A start before previous end");
                    continue;
                }
                impl_->fuBuffer.clear();
                impl_->fuActive = true;
                impl_->fuNalType = type;
                impl_->fuIndicator = payload[0] & 0xE0;
                // The original NAL header byte: F/NRI from the FU indicator,
                // type from the FU header.
                impl_->fuBuffer.push_back(static_cast<uint8_t>((payload[0] & 0xE0) | type));
            } else if (!impl_->fuActive) {
                // Continuation of a fragment whose start was lost -- unusable.
                impl_->DiscardTimestamp(timestamp, "FU-A continuation without start");
                continue;
            } else if (type != impl_->fuNalType || (payload[0] & 0xE0) != impl_->fuIndicator) {
                impl_->DiscardTimestamp(timestamp, "FU-A fragment header changed");
                continue;
            }
            impl_->fuBuffer.insert(impl_->fuBuffer.end(), fragment, fragment + fragmentSize);
            if (!end) continue;

            const uint8_t fuType = impl_->fuNalType;
            if (!impl_->AppendNal(impl_->fuBuffer.data(), impl_->fuBuffer.size(), fuType)) {
                impl_->DiscardTimestamp(timestamp, "invalid slice/picture boundary");
                continue;
            }
            impl_->fuActive = false;
            impl_->fuBuffer.clear();
        } else if (nalType == kNalStapA) {
            if (impl_->fuActive) {
                impl_->DiscardTimestamp(timestamp, "STAP-A before fragment end");
                continue;
            }
            // STAP-A: several small NALs in one packet (SPS+PPS is the usual
            // case). Each is appended separately.
            size_t pos = 1;
            while (pos + 2 <= payloadSize) {
                const uint16_t nalSize = static_cast<uint16_t>((payload[pos] << 8) | payload[pos + 1]);
                pos += 2;
                if (nalSize == 0 || pos + nalSize > payloadSize) break;
                const uint8_t type = payload[pos] & 0x1F;
                if (!impl_->AppendNal(payload + pos, nalSize, type)) {
                    impl_->DiscardTimestamp(timestamp, "invalid aggregated slice/picture boundary");
                    break;
                }
                pos += nalSize;
            }
        } else if (nalType >= 1 && nalType <= 23) {
            // Single NAL unit packet.
            const uint8_t type = nalType;
            if (impl_->fuActive) {
                impl_->DiscardTimestamp(timestamp, "NAL before fragment end");
                continue;
            }
            if (!impl_->AppendNal(payload, payloadSize, type)) {
                impl_->DiscardTimestamp(timestamp, "invalid slice/picture boundary");
                continue;
            }
        } else {
            // Not produced by this sender; silently ignoring it could emit a
            // picture with one of its NALs missing despite contiguous RTP seqs.
            impl_->DiscardTimestamp(timestamp, "unsupported packetization");
            continue;
        }

        if (marker) {
            // The marker bit ends the access unit -- the sender sets it on the
            // last packet of every encoded frame.
            impl_->Emit(impl_->unit, emittedSeq, onPacket);
            impl_->unit.clear();
            impl_->fuActive = false;
            impl_->fuBuffer.clear();
        }
    }
}

}  // namespace phonecam::transport
