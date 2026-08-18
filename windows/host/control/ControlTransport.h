#pragma once

// Manages `adb forward` for the phonecam_control abstract socket and a
// bidirectional TCP connection to the forwarded port, per
// docs/wire-protocol.md (control channel framing: 4-byte LE length prefix +
// FlatBuffers ControlEnvelope). Mirrors transport/AdbTransport.h's shape
// (same no-winsock-in-header rule, same connect-retry rationale) but is
// bidirectional and framing-agnostic -- it hands the caller raw envelope
// bytes, encoding/decoding is ControlChannel's job.

#include <cstdint>
#include <functional>
#include <memory>

namespace phonecam::control {

// data is only valid for the duration of the callback.
using ControlMessageCallback = std::function<void(const uint8_t* data, size_t size)>;

class ControlTransport {
public:
    ControlTransport();
    ~ControlTransport();

    ControlTransport(const ControlTransport&) = delete;
    ControlTransport& operator=(const ControlTransport&) = delete;

    // Runs `adb forward tcp:<port> localabstract:phonecam_control` and
    // connects as a TCP client, retrying briefly (same rationale as
    // AdbVideoTransport::Connect).
    bool Connect(uint16_t port = 27184);
    void Disconnect();

    // Writes one length-prefixed message: a 4-byte LE length followed by
    // `size` bytes of `data`. Safe to call from a different thread than
    // RunReceiveLoop (send/recv are independent directions on the same
    // socket).
    bool Send(const uint8_t* data, size_t size);

    // Blocks parsing length-prefixed messages (payload only, prefix
    // stripped) until the connection closes or a framing error occurs.
    void RunReceiveLoop(const ControlMessageCallback& onMessage);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::control
