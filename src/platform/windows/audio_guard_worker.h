// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace satellite {
namespace audioguard {

struct RestorePass {
    int restored = 0;
    int failed = 0;
    bool keepPolling = false;
};

class GuardBackend {
  public:
    virtual ~GuardBackend() = default;
    virtual void snapshotBeforePlug() = 0;
    virtual RestorePass restoreIfStolen() = 0;
};

struct GuardTiming {
    std::chrono::milliseconds snapshotDeadline{2000};
    std::chrono::milliseconds pollInterval{250};
    int pollBudget = 20;
};

class GuardWorker {
  public:
    using EnabledFn = std::function<bool()>;

    GuardWorker(GuardBackend& backend, EnabledFn enabled = {}, GuardTiming timing = {});
    ~GuardWorker();
    GuardWorker(const GuardWorker&) = delete;
    GuardWorker& operator=(const GuardWorker&) = delete;

    void beforeCompositePlug();
    void afterCompositePlug();

  private:
    void run();
    void pollWindow();

    GuardBackend& backend_;
    EnabledFn enabled_;
    GuardTiming timing_;
    std::mutex mtx_;
    std::condition_variable wake_;
    std::condition_variable replied_;
    uint64_t requested_ = 0;
    uint64_t served_ = 0;
    bool armed_ = false;
    bool windowOpen_ = false;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace audioguard
} // namespace satellite
