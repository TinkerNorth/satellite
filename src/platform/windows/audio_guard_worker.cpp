// SPDX-License-Identifier: LGPL-3.0-or-later
#include "audio_guard_worker.h"

#include "core/tracked_mutex.h"

#include <utility>

namespace satellite {
namespace audioguard {

GuardWorker::GuardWorker(GuardBackend& backend, EnabledFn enabled, GuardTiming timing)
    : backend_(backend), enabled_(std::move(enabled)), timing_(timing) {
    thread_ = std::thread([this] { run(); });
}

GuardWorker::~GuardWorker() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    wake_.notify_all();
    replied_.notify_all();
    blockingRegion("thread.join");
    if (thread_.joinable()) thread_.join();
}

void GuardWorker::beforeCompositePlug() {
    if (enabled_ && !enabled_()) return;
    std::unique_lock<std::mutex> lk(mtx_);
    if (windowOpen_ || stop_) return;
    const uint64_t ticket = ++requested_;
    wake_.notify_one();
    blockingRegion("audio.com");
    replied_.wait_for(lk, timing_.snapshotDeadline, [&] { return served_ >= ticket || stop_; });
}

void GuardWorker::afterCompositePlug() {
    if (enabled_ && !enabled_()) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        armed_ = true;
        windowOpen_ = true;
    }
    wake_.notify_one();
}

void GuardWorker::run() {
    ScopedThreadRole role(ThreadRole::Background);
    std::unique_lock<std::mutex> lk(mtx_);
    while (true) {
        wake_.wait(lk, [this] { return armed_ || stop_ || requested_ > served_; });
        if (stop_) return;
        if (requested_ > served_) {
            const uint64_t ticket = requested_;
            lk.unlock();
            backend_.snapshotBeforePlug();
            lk.lock();
            served_ = ticket;
            replied_.notify_all();
            continue;
        }
        armed_ = false;
        lk.unlock();
        pollWindow();
        lk.lock();
        if (stop_) return;
        if (!armed_) windowOpen_ = false;
    }
}

void GuardWorker::pollWindow() {
    for (int i = 0; i < timing_.pollBudget; i++) {
        const RestorePass pass = backend_.restoreIfStolen();
        if (!pass.keepPolling) break;
        std::unique_lock<std::mutex> wait(mtx_);
        if (wake_.wait_for(wait, timing_.pollInterval, [this] { return armed_ || stop_; })) break;
    }
}

} // namespace audioguard
} // namespace satellite
