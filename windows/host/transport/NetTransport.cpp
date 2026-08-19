#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include "transport/NetTransport.h"

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

#include "log/Log.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace phonecam::transport {

namespace {

constexpr uint8_t kMagic = 0x9C;
constexpr size_t kHeaderSize = 20;
constexpr uint16_t kPort = 27183;

// Deliberately a near-copy of AdbTransport.cpp's equivalents rather than a shared helper: the two
// differ only in how they obtain a host (adb forward to localhost vs. discovery), and AoaTransport
// already carries its own third variant with a different framing prelude. Consolidating all three
// is worth doing if the wire format ever changes; it isn't worth destabilizing two working
// transports to do it pre-emptively.
bool ReadExact(SOCKET s, uint8_t* buffer, size_t size) {
    size_t total = 0;
    while (total < size) {
        const int n = recv(s, reinterpret_cast<char*>(buffer + total), static_cast<int>(size - total), 0);
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

bool DescriptionLooksLikeUsbTethering(const wchar_t* description) {
    if (!description) return false;
    std::wstring lower(description);
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    // Windows names the inbox driver "Remote NDIS Compatible Device" (confirmed live on the
    // reference machine); newer Android gadgets may present NCM instead, which Windows surfaces
    // with "NCM" in the description. Matching on the description rather than VID/PID keeps this
    // working across phone vendors, which is the whole point of using a standard class driver.
    return lower.find(L"ndis") != std::wstring::npos || lower.find(L"ncm") != std::wstring::npos;
}

}  // namespace

std::string DiscoverTetheredPhoneAddress() {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;

    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                   reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) {
        return {};
    }

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (!DescriptionLooksLikeUsbTethering(adapter->Description)) continue;
        // A gateway is what identifies the phone itself on this link -- an RNDIS adapter that
        // came up without one (no DHCP lease yet) has nothing to connect to, so skip rather than
        // guessing an address from the local subnet.
        for (auto* gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next) {
            if (gateway->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* addr = reinterpret_cast<const sockaddr_in*>(gateway->Address.lpSockaddr);
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text))) {
                return std::string(text);
            }
        }
    }
    return {};
}

struct NetVideoTransport::Impl {
    std::string host;  // empty == rediscover per Connect()
    SOCKET sock = INVALID_SOCKET;
    bool wsaOk = false;
    // Guards sock, and the cancelled flag lets a Disconnect() arriving mid-Connect() win rather
    // than being silently lost -- identical reasoning to AdbVideoTransport::Impl, see its
    // comments.
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
};

NetVideoTransport::NetVideoTransport(std::string host) : impl_(std::make_unique<Impl>()) {
    impl_->host = std::move(host);
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        phonecam::log::Error("NetVideoTransport: WSAStartup failed");
        return;
    }
    impl_->wsaOk = true;
}

NetVideoTransport::~NetVideoTransport() {
    Disconnect();
    if (impl_->wsaOk) {
        WSACleanup();
    }
}

bool NetVideoTransport::Connect() {
    if (!impl_->wsaOk) return false;
    impl_->cancelled.store(false);

    const std::string host = impl_->host.empty() ? DiscoverTetheredPhoneAddress() : impl_->host;
    if (host.empty()) {
        // No tethered phone right now. Non-fatal and expected (tethering off, or unplugged) --
        // the supervise loop retries, same as adb failing to find a streaming phone.
        return false;
    }
    if (impl_->cancelled.load()) return false;

    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        phonecam::log::Error("NetVideoTransport: socket() failed");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        phonecam::log::Error(std::format("NetVideoTransport: bad host address '{}'", host));
        closesocket(s);
        return false;
    }

    // Single attempt, not AdbVideoTransport's 20x250ms retry: there, `adb forward` can return
    // before the phone-side listener is accepting, so retrying in-place is the fix. Here a refused
    // connect means the phone app simply isn't capturing yet, which the supervise loop's own
    // backoff already handles -- retrying here too would just hold this call for 5s per attempt
    // and make Disconnect() sluggish.
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return false;
    }

    // Same reasoning as the phone side setting tcpNoDelay: never let Nagle hold a small frame
    // tail back waiting to coalesce. This direction carries almost nothing, but the option also
    // applies to the ACK cadence that shapes the phone's send timing.
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->cancelled.load()) {
        closesocket(s);
        return false;
    }
    impl_->sock = s;
    phonecam::log::Info(std::format("NetVideoTransport: connected to {}:{}", host, kPort));
    return true;
}

void NetVideoTransport::Disconnect() {
    impl_->cancelled.store(true);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sock != INVALID_SOCKET) {
        // shutdown() before closesocket(): the documented-safe way to unblock a concurrent recv()
        // on this socket from RunReceiveLoop's thread (see AdbVideoTransport::Disconnect).
        shutdown(impl_->sock, SD_BOTH);
        closesocket(impl_->sock);
        impl_->sock = INVALID_SOCKET;
    }
}

void NetVideoTransport::RunReceiveLoop(const VideoPacketCallback& onPacket) {
    std::vector<uint8_t> payload;
    uint8_t header[kHeaderSize];

    while (impl_->sock != INVALID_SOCKET) {
        if (!ReadExact(impl_->sock, header, kHeaderSize)) break;

        if (header[0] != kMagic) {
            phonecam::log::Error("NetVideoTransport: bad magic byte, dropping connection");
            break;
        }
        const uint8_t type = header[1];
        const uint8_t flags = header[2];
        // x86/x64 is little-endian natively, matching the wire format's LE spec -- a plain memcpy
        // is correct as-is, no byte-swap needed.
        uint32_t seq = 0, payloadLen = 0;
        uint64_t ptsUs = 0;
        std::memcpy(&seq, header + 4, sizeof(seq));
        std::memcpy(&ptsUs, header + 8, sizeof(ptsUs));
        std::memcpy(&payloadLen, header + 16, sizeof(payloadLen));

        payload.resize(payloadLen);
        if (payloadLen > 0 && !ReadExact(impl_->sock, payload.data(), payloadLen)) break;

        const VideoPacket pkt{
            type == 0 ? PacketType::Config : PacketType::Frame, (flags & 0x01) != 0, seq, ptsUs, payload.data(),
            payload.size(),
        };
        onPacket(pkt);
    }
}

}  // namespace phonecam::transport
