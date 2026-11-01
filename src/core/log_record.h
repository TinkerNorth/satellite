// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/types.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace satellite {

inline constexpr size_t LOG_RECORD_BYTES = 512;
inline constexpr size_t LOG_SOURCE_CAPACITY = 31;
inline constexpr size_t LOG_RECORD_HEADER_BYTES =
    sizeof(int64_t) + sizeof(uint16_t) + sizeof(LogLevel) + 2 * sizeof(uint8_t);
inline constexpr size_t LOG_MESSAGE_CAPACITY =
    LOG_RECORD_BYTES - LOG_RECORD_HEADER_BYTES - LOG_SOURCE_CAPACITY;
inline constexpr std::string_view LOG_TRUNCATION_MARK = "...";
inline constexpr uint8_t LOG_RECORD_RING_ONLY = 0x01;

struct LogRecord {
    int64_t timestampNs = 0;
    uint16_t messageLength = 0;
    LogLevel level = LogLevel::INFO;
    uint8_t sourceLength = 0;
    uint8_t flags = 0;
    std::array<char, LOG_SOURCE_CAPACITY> source{};
    std::array<char, LOG_MESSAGE_CAPACITY> message{};

    std::string_view sourceText() const { return {source.data(), sourceLength}; }
    std::string_view messageText() const { return {message.data(), messageLength}; }
    bool ringOnly() const { return (flags & LOG_RECORD_RING_ONLY) != 0; }
};

static_assert(sizeof(LogLevel) == 1);
static_assert(sizeof(LogRecord) == LOG_RECORD_BYTES);
static_assert(std::is_trivially_copyable_v<LogRecord>);
static_assert(LOG_MESSAGE_CAPACITY <= UINT16_MAX);

inline size_t utf8Prefix(std::string_view text, size_t limit) {
    if (text.size() <= limit) return text.size();
    size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
    return cut;
}

inline int64_t logClockNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline LogRecord makeLogRecord(int64_t timestampNs, LogLevel level, std::string_view source,
                               std::string_view message, uint8_t flags = 0) {
    LogRecord record;
    record.timestampNs = timestampNs;
    record.level = level;
    record.flags = flags;
    const size_t sourceBytes =
        std::min(utf8Prefix(source, LOG_SOURCE_CAPACITY), LOG_SOURCE_CAPACITY);
    if (sourceBytes != 0) std::memcpy(record.source.data(), source.data(), sourceBytes);
    record.sourceLength = static_cast<uint8_t>(sourceBytes);
    size_t messageBytes = std::min(message.size(), LOG_MESSAGE_CAPACITY);
    if (message.size() > LOG_MESSAGE_CAPACITY) {
        const size_t room = LOG_MESSAGE_CAPACITY - LOG_TRUNCATION_MARK.size();
        messageBytes = std::min(utf8Prefix(message, room), room);
    }
    if (messageBytes != 0) std::memcpy(record.message.data(), message.data(), messageBytes);
    if (message.size() > LOG_MESSAGE_CAPACITY) {
        std::memcpy(record.message.data() + messageBytes, LOG_TRUNCATION_MARK.data(),
                    LOG_TRUNCATION_MARK.size());
        messageBytes += LOG_TRUNCATION_MARK.size();
    }
    record.messageLength = static_cast<uint16_t>(messageBytes);
    return record;
}

} // namespace satellite
