// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <atomic>
#include <semaphore>

namespace satellite {

class WorkerWake {
  public:
    void notify() noexcept {
        if (sleeping_.load(std::memory_order_seq_cst) &&
            sleeping_.exchange(false, std::memory_order_seq_cst)) {
            wake_.release();
        }
    }

    template <typename HasWork> void sleepUnless(HasWork&& hasWork) {
        sleeping_.store(true, std::memory_order_seq_cst);
        if (!hasWork()) wake_.acquire();
        sleeping_.store(false, std::memory_order_seq_cst);
    }

  private:
    std::counting_semaphore<> wake_{0};
    std::atomic<bool> sleeping_{false};
};

} // namespace satellite
