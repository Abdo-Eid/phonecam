#pragma once

// Finds a PhoneCam on the local network.
//
// The problem this solves: a wireless session needs the phone's address, and
// making the user type an IP address is the single thing that stops the
// wireless path from feeling like a product. The phone answers a probe
// (see the Android side's DiscoveryResponder), so the PC asks when it needs
// an address rather than the phone advertising itself continuously.
//
// Request/response, not a beacon, on purpose: a phone that isn't being used
// costs nothing on the network, and the PC controls the timing.
//
// The wire format is a line of text, because this is not a hot path and
// legibility is worth more than four bytes:
//
//   PC  -> phone:  "PHONECAM_DISCOVER\n"          (UDP broadcast)
//   phone -> PC:   "PHONECAM_HERE <version> <control-port> <video-port>\n"
//
// The phone's address is taken from the reply's source address rather than
// from the payload, so a phone can't claim to be somewhere it isn't.

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace phonecam::transport {

// Must match the Android side's DiscoveryResponder constants.
inline constexpr uint16_t kDiscoveryPort = 27186;
inline constexpr char kDiscoveryProbe[] = "PHONECAM_DISCOVER";
inline constexpr char kDiscoveryReply[] = "PHONECAM_HERE";

struct DiscoveredPhone {
    std::string address;
    uint16_t protocolVersion = 0;
    uint16_t controlPort = 0;
    uint16_t videoPort = 0;
};

// Broadcasts a probe and waits up to timeoutMs for a phone to answer. Returns
// true when one did. The first answer wins: on a network with more than one
// phone, they are equivalent for our purposes, and picking the first keeps
// this simple and predictable.
bool DiscoverPhone(int timeoutMs, DiscoveredPhone& out);

// Collect/deduplicate all replies for a picker; cancellation is checked between short reads.
std::vector<DiscoveredPhone> DiscoverPhones(int timeoutMs, const std::function<bool()>& cancelled = {});
struct QrPairedPhone { DiscoveredPhone endpoint; std::string id, name, key; };
bool PairQrPhone(const std::string& pcId, const std::string& token, int timeoutMs, QrPairedPhone& out,
                  const std::function<bool()>& cancelled);
bool DiscoverPairedPhone(const std::string& pcId, const std::string& phoneId, const std::string& key,
                          int timeoutMs, DiscoveredPhone& out, const std::function<bool()>& cancelled);

}  // namespace phonecam::transport
