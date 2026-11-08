#pragma once

// Video over a plain TCP connection to the phone on USB tethering, with no adb
// and no custom USB driver.
//
// Why this exists alongside AdbVideoTransport: enabling USB tethering makes
// the phone present a network interface, which Windows binds with its own *inbox* RNDIS driver
// automatically -- no install, no UAC, and nothing displaced. Measured live on
// the reference device: 3-5ms round-trip, i.e. USB latency.
//
// Address selection is deterministic rather than mDNS/manual: over tethering the phone *is* the PC's
// default gateway on that link, so finding it is "enumerate adapters, find the RNDIS/NCM one,
// read its gateway".
//
// No winsock/windows types in this header, same rule as AdbTransport.h: it's included from
// main.cpp alongside <mfapi.h>, and winsock2.h must be the first Windows header in a translation
// unit or windows.h's default winsock.h pull-in conflicts with it.

#include <cstdint>
#include <memory>
#include <string>

#include "transport/VideoTransport.h"

namespace phonecam::transport {

// Returns the IPv4 address of the phone at the far end of a USB-tethering link (the RNDIS/NCM
// adapter's default gateway), or an empty string if no such adapter is present. Cheap, no
// elevation, safe to call repeatedly -- used both for transport auto-detection and by
// NetVideoTransport's own connect path.
std::string DiscoverTetheredPhoneAddress();

class NetVideoTransport : public VideoTransport {
public:
    // host empty (the default) means "re-discover the tethered phone on every Connect()" -- doing
    // it per-connect rather than once in the constructor is what lets the reconnect loop recover
    // after a replug, when the RNDIS link comes back with a different address.
    explicit NetVideoTransport(std::string host = {});
    ~NetVideoTransport() override;

    NetVideoTransport(const NetVideoTransport&) = delete;
    NetVideoTransport& operator=(const NetVideoTransport&) = delete;

    void SetPort(uint16_t port);

    bool Connect() override;
    void Disconnect() override;
    void RunReceiveLoop(const VideoPacketCallback& onPacket) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::transport
