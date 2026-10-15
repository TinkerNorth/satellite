// SPDX-License-Identifier: LGPL-3.0-or-later
#include "net/receive_wake.h"

#include <chrono>

namespace {

constexpr int WAKE_ATTEMPTS = 100;
constexpr auto WAKE_RETRY = std::chrono::milliseconds(10);

} // namespace

void ReceiveWake::arm(SOCKET sock, uint16_t port) {
    std::lock_guard<std::mutex> lk(mtx_);
    sock_ = sock;
    port_ = port;
}

void ReceiveWake::disarm() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        sock_ = INVALID_SOCKET;
    }
    disarmed_.notify_all();
}

void ReceiveWake::wake() {
    std::unique_lock<std::mutex> lk(mtx_);
    if (sock_ == INVALID_SOCKET) return;
    sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_port = htons(port_);
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(sock_, "", 0, 0, reinterpret_cast<const sockaddr*>(&self), sizeof(self));
    for (int attempt = 0; attempt < WAKE_ATTEMPTS; ++attempt) {
        if (disarmed_.wait_for(lk, WAKE_RETRY, [this] { return sock_ == INVALID_SOCKET; })) return;
#ifdef _WIN32
        CancelIoEx(reinterpret_cast<HANDLE>(sock_), nullptr);
#endif
    }
}
