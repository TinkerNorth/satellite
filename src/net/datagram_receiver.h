// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "net/net_compat.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class DatagramReceiver {
  public:
    DatagramReceiver(SOCKET sock, std::size_t capacity, unsigned timeoutMs);
    ~DatagramReceiver();
    DatagramReceiver(const DatagramReceiver&) = delete;
    DatagramReceiver& operator=(const DatagramReceiver&) = delete;

    int receive();
    uint8_t* data() { return buf_.data(); }
    const sockaddr_in& sender() const { return from_; }

  private:
    SOCKET sock_;
    std::vector<uint8_t> buf_;
    sockaddr_in from_{};
    socklen_t fromLen_ = sizeof(from_);
#ifdef _WIN32
    unsigned timeoutMs_;
    WSAOVERLAPPED ov_{};
    DWORD flags_ = 0;
    bool pending_ = false;

    bool start();
#endif
};
