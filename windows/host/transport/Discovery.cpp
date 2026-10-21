#include "transport/Discovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "log/Log.h"
#include "control/ControlChannel.h"
#include "connection/PairingStore.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace phonecam::transport {

namespace {

// The limited broadcast address, plus every adapter's directed broadcast.
//
// Both matter: the limited broadcast (255.255.255.255) is what a phone on
// the same link usually receives, but a directed broadcast (e.g.
// 192.168.137.255 on a hotspot) is more reliably delivered on some Windows
// configurations. Sending to both costs one extra datagram and removes a
// class of "it works on my machine" failure.
std::vector<std::string> BroadcastAddresses() {
    std::vector<std::string> addresses;
    addresses.emplace_back("255.255.255.255");

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
    if (rc != NO_ERROR) return addresses;

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* addr = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            // Host order on both sides: the address is byte-swapped out of the socket
            // structure, and the mask is built from the prefix length here, so it is
            // already host order. (Byte-swapping the mask as well produced nonsense
            // addresses -- 255.168.1.45 instead of 192.168.1.255 -- and the probes went
            // nowhere.)
            const uint32_t ip = ntohl(addr->sin_addr.s_addr);
            const uint8_t prefix = unicast->OnLinkPrefixLength;
            const uint32_t mask = prefix == 0 ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - prefix));
            if (mask == 0) continue;
            const uint32_t broadcast = (ip & mask) | ~mask;
            char text[INET_ADDRSTRLEN]{};
            struct in_addr asInAddr{};
            asInAddr.s_addr = htonl(broadcast);
            if (inet_ntop(AF_INET, &asInAddr, text, sizeof(text))) {
                addresses.emplace_back(text);
            }
            break;  // one address per adapter is enough
        }
    }
    return addresses;
}

bool ParseReply(const std::string& text, DiscoveredPhone& out) {
    if (text.rfind(kDiscoveryReply, 0) != 0) return false;
    // Parsed by hand rather than with sscanf: the latter is deprecated in the
    // secure CRT (and the project builds warning-clean).
    std::istringstream stream(text.substr(std::string(kDiscoveryReply).size()));
    int version = 0;
    int controlPort = 0;
    int videoPort = 0;
    if (!(stream >> version >> controlPort >> videoPort)) return false;
    if (version < 0 || version > 0xFFFF || controlPort <= 0 || controlPort > 0xFFFF || videoPort <= 0 ||
        videoPort > 0xFFFF) {
        return false;
    }
    out.protocolVersion = static_cast<uint16_t>(version);
    out.controlPort = static_cast<uint16_t>(controlPort);
    out.videoPort = static_cast<uint16_t>(videoPort);
    return true;
}

}  // namespace

std::vector<DiscoveredPhone> DiscoverPhones(int timeoutMs, const std::function<bool()>& cancelled) {
    std::vector<DiscoveredPhone> phones;
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        phonecam::log::Error("Discovery: WSAStartup failed");
        return phones;
    }

    const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        phonecam::log::Error("Discovery: socket() failed");
        WSACleanup();
        return phones;
    }
    BOOL broadcast = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
    // Bound to an ephemeral port: the phone's reply is a unicast datagram to
    // wherever the probe came from, and the source address of the probe is the
    // PC's real address (not a broadcast one) as long as the socket is bound to
    // a real interface.
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        phonecam::log::Error("Discovery: bind() failed");
        closesocket(sock);
        WSACleanup();
        return phones;
    }

    const std::string probe = std::string(kDiscoveryProbe) + "\n";
    const auto broadcasts = BroadcastAddresses();
    for (const auto& target : broadcasts) {
        phonecam::log::Info("Discovery: probing " + target);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const auto stopped = [&]() { return cancelled && cancelled(); };
    while (!stopped() && std::chrono::steady_clock::now() < deadline) {
        // Re-send periodically: a datagram can be lost, and the phone may not
        // have been listening yet when the first one went out.
        for (const auto& target : broadcasts) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(kDiscoveryPort);
            if (inet_pton(AF_INET, target.c_str(), &addr.sin_addr) != 1) continue;
            const int sent = sendto(sock, probe.data(), static_cast<int>(probe.size()), 0,
                                    reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (sent == SOCKET_ERROR) {
                phonecam::log::Error("Discovery: probe to " + target + " failed, error " +
                                     std::to_string(WSAGetLastError()));
            }
        }

        // Collect replies until the deadline.
        while (!stopped() && std::chrono::steady_clock::now() < deadline) {
            DWORD timeout = 250;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            char buffer[256];
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&from),
                                          &fromLen);
            if (received <= 0) break;  // timed out: send another probe

            std::string text(buffer, static_cast<size_t>(received));
            phonecam::log::Info("Discovery: got a reply: " + text);
            DiscoveredPhone candidate;
            if (!ParseReply(text, candidate)) continue;

            // The source address is authoritative -- a phone can't claim to be
            // somewhere it isn't.
            char address[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address)) == nullptr) continue;
            candidate.address = address;
            bool duplicate = false;
            for (const auto& phone : phones) {
                if (phone.address == candidate.address && phone.controlPort == candidate.controlPort) duplicate = true;
            }
            if (!duplicate) phones.push_back(candidate);
        }
    }

    closesocket(sock);
    WSACleanup();
    return phones;
}

bool DiscoverPhone(int timeoutMs, DiscoveredPhone& out) {
    auto phones = DiscoverPhones(timeoutMs);
    if (phones.empty()) return false;
    out = phones.front();
    phonecam::log::Info("Discovery: found a PhoneCam at " + out.address + " (protocol v" +
                         std::to_string(out.protocolVersion) + ", control port " +
                         std::to_string(out.controlPort) + ", video port " + std::to_string(out.videoPort) + ")");
    return true;
}

namespace {
bool Hex(const std::string& value, size_t size) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool SameProof(const std::string& a, const std::string& b) {
    if (a.size() != 64 || b.size() != 64) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned>(a[i] ^ b[i]);
    return diff == 0;
}
std::string Unhex(const std::string& value) {
    if (value.size() > 256 || value.size() % 2 || value.find_first_not_of("0123456789abcdef") != std::string::npos) return {};
    std::string result;
    for (size_t i = 0; i < value.size(); i += 2) result.push_back(static_cast<char>(std::stoi(value.substr(i, 2), nullptr, 16)));
    return result;
}
// The existing discovery port carries pairing; no new PC listening port/firewall rule.
bool Exchange(const std::string& probe, int timeoutMs, const std::function<bool()>& cancelled,
              const std::function<bool(SOCKET, const sockaddr_in&, const std::string&)>& reply) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { WSACleanup(); return false; }
    BOOL broadcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&broadcast), sizeof(broadcast));
    DWORD timeout = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    sockaddr_in local{}; local.sin_family = AF_INET;
    if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) { closesocket(s); WSACleanup(); return false; }
    const auto addresses = BroadcastAddresses();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool found = false;
    while (!cancelled() && std::chrono::steady_clock::now() < deadline && !found) {
        for (const auto& address : addresses) {
            sockaddr_in target{}; target.sin_family = AF_INET; target.sin_port = htons(kDiscoveryPort);
            if (inet_pton(AF_INET, address.c_str(), &target.sin_addr) == 1)
                sendto(s, probe.data(), static_cast<int>(probe.size()), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
        }
        for (int i = 0; i < 8 && !cancelled() && std::chrono::steady_clock::now() < deadline; ++i) {
            char bytes[1024]; sockaddr_in from{}; int size = sizeof(from);
            const int count = recvfrom(s, bytes, sizeof(bytes), 0, reinterpret_cast<sockaddr*>(&from), &size);
            if (count <= 0) break;
            if (reply(s, from, std::string(bytes, count))) { found = true; break; }
        }
    }
    closesocket(s); WSACleanup(); return found;
}
std::string Address(const sockaddr_in& from) {
    char address[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));
    return address;
}
}

bool PairQrPhone(const std::string& pcId, const std::string& token, int timeoutMs, QrPairedPhone& out,
                  const std::function<bool()>& cancelled) {
    if (!Hex(pcId, 32) || !Hex(token, 64)) return false;
    const auto challenge = phonecam::connection::RandomHex(16);
    if (challenge.empty()) return false;
    std::string phoneId, phoneNonce, key;
    sockaddr_in offered{};
    return Exchange("PHONECAM_PAIR " + pcId + " " + challenge + "\n", timeoutMs, cancelled,
        [&](SOCKET socket, const sockaddr_in& from, const std::string& text) {
            std::istringstream input(text);
            std::string kind; input >> kind;
            if (kind == "PHONECAM_OFFER") {
                std::string id, nonce, nameHex, proof;
                int control = 0, video = 0;
                if (!(input >> id >> nonce >> control >> video >> nameHex >> proof) || !Hex(id, 32) || !Hex(nonce, 32) ||
                    control < 1 || control > 65535 || video < 1 || video > 65535) return false;
                const auto candidateKey = phonecam::control::ComputeMessageHmac(token, "pair-key|" + pcId + "|" + id + "|" + nonce);
                const auto expected = phonecam::control::ComputeMessageHmac(candidateKey, "offer|" + pcId + "|" + id + "|" + nonce + "|" +
                    challenge + "|" + std::to_string(control) + "|" + std::to_string(video) + "|" + nameHex);
                if (!SameProof(proof, expected)) return false;
                phoneId = id; phoneNonce = nonce; key = candidateKey; offered = from;
                out.id = id; out.key = key; out.name = Unhex(nameHex);
                out.endpoint = {Address(from), 6, static_cast<uint16_t>(control), static_cast<uint16_t>(video)};
                const auto confirm = "PHONECAM_CONFIRM " + pcId + " " + id + " " + nonce + " " + challenge + " " +
                    phonecam::control::ComputeMessageHmac(key, "confirm|" + pcId + "|" + id + "|" + nonce + "|" + challenge) + "\n";
                sendto(socket, confirm.data(), static_cast<int>(confirm.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
            } else if (kind == "PHONECAM_PAIRED" && !key.empty() && from.sin_addr.s_addr == offered.sin_addr.s_addr) {
                std::string id, proof; input >> id >> proof;
                return id == phoneId && SameProof(proof, phonecam::control::ComputeMessageHmac(key,
                    "paired|" + pcId + "|" + phoneId + "|" + phoneNonce + "|" + challenge));
            }
            return false;
        });
}

bool DiscoverPairedPhone(const std::string& pcId, const std::string& phoneId, const std::string& key,
                          int timeoutMs, DiscoveredPhone& out, const std::function<bool()>& cancelled) {
    const auto challenge = phonecam::connection::RandomHex(16);
    if (!Hex(pcId, 32) || !Hex(phoneId, 32) || !Hex(key, 64) || challenge.empty()) return false;
    return Exchange("PHONECAM_KNOWN " + pcId + " " + challenge + "\n", timeoutMs, cancelled,
        [&](SOCKET, const sockaddr_in& from, const std::string& text) {
            std::istringstream input(text);
            std::string kind, id, proof; int control = 0, video = 0;
            if (!(input >> kind >> id >> control >> video >> proof) || kind != "PHONECAM_KNOWN_HERE" || id != phoneId ||
                control < 1 || control > 65535 || video < 1 || video > 65535) return false;
            if (!SameProof(proof, phonecam::control::ComputeMessageHmac(key, "known|" + pcId + "|" + id + "|" + challenge + "|" +
                std::to_string(control) + "|" + std::to_string(video)))) return false;
            out = {Address(from), 6, static_cast<uint16_t>(control), static_cast<uint16_t>(video)};
            return true;
        });
}

}  // namespace phonecam::transport
