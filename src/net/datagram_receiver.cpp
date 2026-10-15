// SPDX-License-Identifier: LGPL-3.0-or-later
#include "net/datagram_receiver.h"

#ifdef _WIN32

DatagramReceiver::DatagramReceiver(SOCKET sock, std::size_t capacity, unsigned timeoutMs)
    : sock_(sock), buf_(capacity), timeoutMs_(timeoutMs) {
    ov_.hEvent = WSACreateEvent();
}

DatagramReceiver::~DatagramReceiver() {
    if (pending_) {
        CancelIoEx(reinterpret_cast<HANDLE>(sock_), &ov_);
        WaitForSingleObject(ov_.hEvent, INFINITE);
    }
    if (ov_.hEvent != WSA_INVALID_EVENT) WSACloseEvent(ov_.hEvent);
}

bool DatagramReceiver::start() {
    WSAResetEvent(ov_.hEvent);
    WSABUF wb{static_cast<ULONG>(buf_.size()), reinterpret_cast<char*>(buf_.data())};
    flags_ = 0;
    fromLen_ = sizeof(from_);
    if (WSARecvFrom(sock_, &wb, 1, nullptr, &flags_, reinterpret_cast<sockaddr*>(&from_), &fromLen_,
                    &ov_, nullptr) == SOCKET_ERROR &&
        WSAGetLastError() != WSA_IO_PENDING) {
        return false;
    }
    pending_ = true;
    return true;
}

int DatagramReceiver::receive() {
    if (ov_.hEvent == WSA_INVALID_EVENT) {
        netSleepMs(timeoutMs_);
        return -1;
    }
    if (!pending_ && !start()) return -1;
    if (WaitForSingleObject(ov_.hEvent, timeoutMs_) != WAIT_OBJECT_0) return -1;
    DWORD bytes = 0;
    DWORD flags = 0;
    const bool ok = WSAGetOverlappedResult(sock_, &ov_, &bytes, FALSE, &flags) != FALSE;
    if (!ok && WSAGetLastError() == WSA_IO_INCOMPLETE) return -1;
    pending_ = false;
    return ok ? static_cast<int>(bytes) : -1;
}

#else

DatagramReceiver::DatagramReceiver(SOCKET sock, std::size_t capacity, unsigned timeoutMs)
    : sock_(sock), buf_(capacity) {
    netSetRecvTimeoutMs(sock, timeoutMs);
}

DatagramReceiver::~DatagramReceiver() = default;

int DatagramReceiver::receive() {
    fromLen_ = sizeof(from_);
    return static_cast<int>(recvfrom(sock_, buf_.data(), buf_.size(), 0,
                                     reinterpret_cast<sockaddr*>(&from_), &fromLen_));
}

#endif
