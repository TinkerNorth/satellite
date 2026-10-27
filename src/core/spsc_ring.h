// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/mpsc_queue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace satellite {

template <typename T, size_t Capacity> class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);

  public:
    static constexpr size_t CAPACITY = Capacity;

    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    T* claim() noexcept {
        const uint64_t head = head_.value.load(std::memory_order_relaxed);
        if (head - tail_.value.load(std::memory_order_acquire) >= Capacity) return nullptr;
        return &cells_[static_cast<size_t>(head & MASK)];
    }

    void publish() noexcept {
        head_.value.store(head_.value.load(std::memory_order_relaxed) + 1,
                          std::memory_order_seq_cst);
    }

    bool push(const T& item) noexcept {
        T* cell = claim();
        if (cell == nullptr) return false;
        *cell = item;
        publish();
        return true;
    }

    const T* front() const noexcept {
        const uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        if (head_.value.load(std::memory_order_seq_cst) == tail) return nullptr;
        return &cells_[static_cast<size_t>(tail & MASK)];
    }

    void release() noexcept {
        tail_.value.store(tail_.value.load(std::memory_order_relaxed) + 1,
                          std::memory_order_release);
    }

    bool pop(T& out) noexcept {
        const T* cell = front();
        if (cell == nullptr) return false;
        out = *cell;
        release();
        return true;
    }

    uint64_t pushed() const noexcept { return head_.value.load(std::memory_order_seq_cst); }

    uint64_t popped() const noexcept { return tail_.value.load(std::memory_order_acquire); }

    bool empty() const noexcept { return pushed() == popped(); }

  private:
    static constexpr uint64_t MASK = Capacity - 1;

    struct Position {
        std::atomic<uint64_t> value{0};
        std::array<char, QUEUE_LINE_BYTES - sizeof(std::atomic<uint64_t>)> padding{};
    };

    std::array<T, Capacity> cells_{};
    Position head_{};
    Position tail_{};
};

} // namespace satellite
