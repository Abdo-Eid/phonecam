#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "transport/AdbTransport.h"

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

#include "log/Log.h"

#pragma comment(lib, "ws2_32.lib")

namespace phonecam::transport {

namespace {

constexpr uint8_t kMagic = 0x9C;
constexpr size_t kHeaderSize = 20;

bool RunAdbForward(uint16_t port) {
    std::wstring cmd = std::format(L"adb forward tcp:{} localabstract:phonecam_video", port);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                    &si, &pi);
    if (!ok) {
        phonecam::log::Error("AdbVideoTransport: failed to launch adb (is it on PATH?)");
        return false;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exitCode != 0) {
        phonecam::log::Error(std::format("AdbVideoTransport: adb forward exited with code {}", exitCode));
        return false;
    }
    return true;
}

// Loops recv() until exactly `size` bytes are read. Returns false on an
// orderly close (0) or a socket error (<0) -- both mean "stop reading."
bool ReadExact(SOCKET s, uint8_t* buffer, size_t size) {
    size_t total = 0;
    while (total < size) {
        const int n = recv(s, reinterpret_cast<char*>(buffer + total), static_cast<int>(size - total), 0);
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

struct AdbVideoTransport::Impl {
    SOCKET sock = INVALID_SOCKET;
    bool wsaOk = false;
    // Guards sock: Phase 5's reconnect loop calls Connect() repeatedly from a
    // worker thread while Stop()/dtor can call Disconnect() from another
    // thread at any time, including mid-Connect() -- unlike Phase 3B, where
    // Connect() only ever ran once, synchronously, before any thread existed.
    std::mutex mutex;
    // Set by Disconnect() before it touches sock, checked by Connect() at its
    // cancellation points, so a Disconnect() that arrives while a connect
    // attempt is in flight (retrying, or holding a not-yet-published SOCKET)
    // is not silently lost -- Connect() will back out and close/discard
    // whatever it was about to publish instead of racing Stop() to leave a
    // live, un-torn-down socket behind after shutdown was requested.
    std::atomic<bool> cancelled{false};
};

AdbVideoTransport::AdbVideoTransport() : impl_(std::make_unique<Impl>()) {
    // WSAStartup/WSACleanup are meant to be paired once per session, not once
    // per connection attempt -- doing that per-Connect()/Disconnect() (as
    // Phase 3B did, when there was only ever one attempt) is unsafe once a
    // reconnect loop can race a fresh Connect() against a Stop()-triggered
    // Disconnect() calling WSACleanup() while the new attempt's socket is
    // still in use.
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        phonecam::log::Error("AdbVideoTransport: WSAStartup failed");
        return;
    }
    impl_->wsaOk = true;
}

AdbVideoTransport::~AdbVideoTransport() {
    Disconnect();
    if (impl_->wsaOk) {
        WSACleanup();
    }
}

bool AdbVideoTransport::Connect(uint16_t port) {
    impl_->cancelled.store(false);
    if (!RunAdbForward(port)) {
        return false;
    }
    if (impl_->cancelled.load()) return false;

    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        phonecam::log::Error("AdbVideoTransport: socket() failed");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // adb forward can return before the phone-side LocalServerSocket is
    // actually accepting yet -- retry briefly rather than failing once.
    bool connected = false;
    for (int attempt = 0; attempt < 20 && !impl_->cancelled.load(); ++attempt) {
        if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            connected = true;
            break;
        }
        Sleep(250);
    }
    if (!connected) {
        closesocket(s);
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->cancelled.load()) {
        // Disconnect() arrived while we were connecting -- don't publish a
        // socket Stop() already believes is torn down.
        closesocket(s);
        return false;
    }
    impl_->sock = s;
    phonecam::log::Info("AdbVideoTransport: connected");
    return true;
}

void AdbVideoTransport::Disconnect() {
    impl_->cancelled.store(true);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sock != INVALID_SOCKET) {
        // shutdown() first: this is the documented-safe way to unblock a
        // concurrent recv() on this socket from another thread (RunReceiveLoop
        // typically runs on its own thread). closesocket() alone, called
        // cross-thread while another thread is blocked in recv() on the same
        // handle, is not reliable on Winsock the way it is on POSIX sockets.
        shutdown(impl_->sock, SD_BOTH);
        closesocket(impl_->sock);
        impl_->sock = INVALID_SOCKET;
    }
}

void AdbVideoTransport::RunReceiveLoop(const VideoPacketCallback& onPacket) {
    std::vector<uint8_t> payload;
    uint8_t header[kHeaderSize];

    while (impl_->sock != INVALID_SOCKET) {
        if (!ReadExact(impl_->sock, header, kHeaderSize)) break;

        if (header[0] != kMagic) {
            phonecam::log::Error("AdbVideoTransport: bad magic byte, dropping connection");
            break;
        }
        const uint8_t type = header[1];
        const uint8_t flags = header[2];
        // Windows only ever runs this on x86/x64 (little-endian natively),
        // matching the wire format's LE spec -- a plain memcpy is correct
        // as-is, no byte-swap needed.
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
