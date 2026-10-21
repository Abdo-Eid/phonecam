#pragma once

// Include after winsock2.h. A cancelled/stale endpoint must not freeze a tray mode switch.
#include <chrono>
#include <functional>

namespace phonecam::transport {
inline bool ConnectSocket(SOCKET socket, const sockaddr_in& address, const std::function<bool()>& cancelled) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) return false;
    bool ok = connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    const int error = WSAGetLastError();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (!ok && (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY)) {
        while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
            fd_set writable, failed;
            FD_ZERO(&writable); FD_SET(socket, &writable);
            FD_ZERO(&failed); FD_SET(socket, &failed);
            timeval timeout{0, 100000};
            const int ready = select(0, nullptr, &writable, &failed, &timeout);
            if (ready < 0) break;
            if (ready > 0) {
                int result = 0;
                int length = sizeof(result);
                ok = getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&result), &length) == 0 && result == 0;
                break;
            }
        }
    }
    nonblocking = 0;
    ioctlsocket(socket, FIONBIO, &nonblocking);
    return ok && !cancelled();
}
}
