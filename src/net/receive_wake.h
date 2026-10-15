// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "net/net_compat.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>

class ReceiveWake {
  public:
    void arm(SOCKET sock, uint16_t port);
    void disarm();
    void wake();

  private:
    std::mutex mtx_;
    std::condition_variable disarmed_;
    SOCKET sock_ = INVALID_SOCKET;
    uint16_t port_ = 0;
};
