// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/log_record.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace satellite {

inline constexpr size_t LOG_QUEUE_CAPACITY = 4096;
inline constexpr size_t LOG_QUEUE_LINE_BYTES = 64;

template <size_t Capacity> class BasicLogQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);

  public:
    static constexpr size_t CAPACITY = Capacity;

    BasicLogQueue() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    BasicLogQueue(const BasicLogQueue&) = delete;
    BasicLogQueue& operator=(const BasicLogQueue&) = delete;

    bool push(const LogRecord& record) noexcept {
        uint64_t pos = enqueuePos_.value.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[static_cast<size_t>(pos & MASK)];
            const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto lag = static_cast<int64_t>(sequence - pos);
            if (lag == 0) {
                if (enqueuePos_.value.compare_exchange_weak(pos, pos + 1, std::memory_order_seq_cst,
                                                            std::memory_order_relaxed)) {
                    cell.record = record;
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (lag < 0) {
                dropped_.value.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                pos = enqueuePos_.value.load(std::memory_order_relaxed);
            }
        }
    }

    bool pop(LogRecord& out) noexcept {
        const uint64_t pos = dequeuePos_.value.load(std::memory_order_relaxed);
        Cell& cell = cells_[static_cast<size_t>(pos & MASK)];
        if (cell.sequence.load(std::memory_order_acquire) != pos + 1) return false;
        out = cell.record;
        cell.sequence.store(pos + Capacity, std::memory_order_release);
        dequeuePos_.value.store(pos + 1, std::memory_order_release);
        return true;
    }

    uint64_t enqueued() const noexcept { return enqueuePos_.value.load(std::memory_order_seq_cst); }

    uint64_t consumed() const noexcept { return dequeuePos_.value.load(std::memory_order_acquire); }

    uint64_t depth() const noexcept {
        const uint64_t head = consumed();
        return enqueued() - head;
    }

    uint64_t dropped() const noexcept { return dropped_.value.load(std::memory_order_relaxed); }

  private:
    static constexpr uint64_t MASK = Capacity - 1;

    struct Cell {
        std::atomic<uint64_t> sequence{0};
        LogRecord record{};
    };

    struct Position {
        std::atomic<uint64_t> value{0};
        std::array<char, LOG_QUEUE_LINE_BYTES - sizeof(std::atomic<uint64_t>)> padding{};
    };

    std::array<Cell, Capacity> cells_{};
    Position enqueuePos_{};
    Position dequeuePos_{};
    Position dropped_{};
};

using LogQueue = BasicLogQueue<LOG_QUEUE_CAPACITY>;

} // namespace satellite
