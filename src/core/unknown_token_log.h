// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/ipv4_util.h"
#include "core/types.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace satellite {

inline constexpr std::chrono::seconds UNKNOWN_TOKEN_QUIET_PERIOD{HEARTBEAT_INTERVAL_SEC *
                                                                 HEARTBEAT_MISS_MAX};

struct UnknownTokenLogLine {
    LogLevel level = LogLevel::INFO;
    std::string message;
};

class UnknownTokenLog {
  public:
    UnknownTokenLogLine observe(uint64_t droppedTotal, uint32_t senderIPv4Nbo,
                                std::chrono::steady_clock::time_point now) {
        UnknownTokenLogLine line;
        if (droppedTotal < lastTotal_) {
            lastTotal_ = droppedTotal;
            stretchOpen_ = false;
            return line;
        }
        const bool newDrops = droppedTotal > lastTotal_;
        const uint64_t before = lastTotal_;
        lastTotal_ = droppedTotal;
        if (newDrops) {
            lastDropAt_ = now;
            if (stretchOpen_) return line;
            stretchOpen_ = true;
            stretchStartTotal_ = before;
            line.level = LogLevel::WARN;
            line.message = openedMessage(senderIPv4Nbo);
            return line;
        }
        const bool quietLongEnough = now - lastDropAt_ >= UNKNOWN_TOKEN_QUIET_PERIOD;
        if (!stretchOpen_ || !quietLongEnough) return line;
        stretchOpen_ = false;
        line.level = LogLevel::INFO;
        line.message = endedMessage(senderIPv4Nbo, droppedTotal - stretchStartTotal_);
        return line;
    }

  private:
    static std::string senderClause(uint32_t senderIPv4Nbo) {
        if (senderIPv4Nbo == 0) return "";
        return " from " + formatIPv4Nbo(senderIPv4Nbo);
    }

    static std::string openedMessage(uint32_t senderIPv4Nbo) {
        return "Dropping datagrams" + senderClause(senderIPv4Nbo) +
               " for a session this Satellite does not have (Satellite restarted, or the "
               "session was reaped); the dish opens a new session on its own once its "
               "heartbeats go unanswered";
    }

    static std::string endedMessage(uint32_t senderIPv4Nbo, uint64_t dropped) {
        return "Datagrams" + senderClause(senderIPv4Nbo) +
               " for a session this Satellite does not have have stopped after " +
               std::to_string(dropped) +
               (dropped == 1 ? " dropped datagram" : " dropped datagrams");
    }

    uint64_t lastTotal_ = 0;
    uint64_t stretchStartTotal_ = 0;
    bool stretchOpen_ = false;
    std::chrono::steady_clock::time_point lastDropAt_{};
};

} // namespace satellite
