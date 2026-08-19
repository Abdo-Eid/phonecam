#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "control/ControlTransport.h"

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

#include "log/Log.h"

#pragma comment(lib, "ws2_32.lib")

namespace phonecam::control {

namespace {

bool RunAdbForward(uint16_t port) {
    std::wstring cmd = std::format(L"adb forward tcp:{} localabstract:phonecam_control", port);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                    &si, &pi);
    if (!ok) {
        phonecam::log::Error("ControlTransport: failed to launch adb (is it on PATH?)");
        return false;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exitCode != 0) {
        phonecam::log::Error(std::format("ControlTransport: adb forward exited with code {}", exitCode));
        return false;
    }
    return true;
}

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

struct ControlTransport::Impl {
    SOCKET sock = INVALID_SOCKET;
    bool wsaOk = false;
    // Same race and same fix as AdbVideoTransport::Impl (see its comments):
    // Phase 5's reconnect loop calls Connect() from a worker thread while
    // Stop()/dtor can call Disconnect() from another thread at any time.
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    // Separate from `mutex` (which guards connect/disconnect of `sock` itself) on purpose: a
    // send racing a concurrent Disconnect() should fail fast on the INVALID_SOCKET check below,
    // not block waiting for a lock an unrelated teardown is holding. Needed now that the tray
    // (Phase 4: lens/zoom/ev/torch/focus menu clicks, on the tray's message-pump thread) is a
    // second concurrent sender alongside ConsoleControlUi's console-input thread -- interleaved
    // length-prefix + body writes from two threads would corrupt framing on the wire.
    std::mutex sendMutex;
};

ControlTransport::ControlTransport() : impl_(std::make_unique<Impl>()) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        phonecam::log::Error("ControlTransport: WSAStartup failed");
        return;
    }
    impl_->wsaOk = true;
}

ControlTransport::~ControlTransport() {
    Disconnect();
    if (impl_->wsaOk) {
        WSACleanup();
    }
}

bool ControlTransport::Connect(uint16_t port) {
    impl_->cancelled.store(false);
    if (!RunAdbForward(port)) {
        return false;
    }
    if (impl_->cancelled.load()) return false;

    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        phonecam::log::Error("ControlTransport: socket() failed");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

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
        closesocket(s);
        return false;
    }
    impl_->sock = s;
    phonecam::log::Info("ControlTransport: connected");
    return true;
}

void ControlTransport::Disconnect() {
    impl_->cancelled.store(true);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sock != INVALID_SOCKET) {
        // shutdown() before closesocket(): the documented-safe way to unblock
        // a concurrent recv() on this socket from RunReceiveLoop's thread
        // (see AdbVideoTransport::Disconnect for the same fix, found in
        // Phase 3B).
        shutdown(impl_->sock, SD_BOTH);
        closesocket(impl_->sock);
        impl_->sock = INVALID_SOCKET;
    }
}

bool ControlTransport::Send(const uint8_t* data, size_t size) {
    if (impl_->sock == INVALID_SOCKET) return false;
    std::lock_guard<std::mutex> sendLock(impl_->sendMutex);
    if (impl_->sock == INVALID_SOCKET) return false;  // re-check: may have disconnected while waiting for the lock
    uint32_t len = static_cast<uint32_t>(size);
    // x86/x64 is natively little-endian, matching the wire spec -- a plain
    // send of the raw bytes is correct as-is (same reasoning as the video
    // transport's header parsing).
    if (send(impl_->sock, reinterpret_cast<const char*>(&len), sizeof(len), 0) != sizeof(len)) return false;
    size_t total = 0;
    while (total < size) {
        const int n = send(impl_->sock, reinterpret_cast<const char*>(data + total),
                            static_cast<int>(size - total), 0);
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

void ControlTransport::RunReceiveLoop(const ControlMessageCallback& onMessage) {
    std::vector<uint8_t> payload;
    uint8_t lenBytes[4];

    while (impl_->sock != INVALID_SOCKET) {
        if (!ReadExact(impl_->sock, lenBytes, sizeof(lenBytes))) break;
        uint32_t len = 0;
        std::memcpy(&len, lenBytes, sizeof(len));

        payload.resize(len);
        if (len > 0 && !ReadExact(impl_->sock, payload.data(), len)) break;

        onMessage(payload.data(), payload.size());
    }
}

}  // namespace phonecam::control
