// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/config_store.h"

#include <algorithm>
#include <string>
#include <utility>

namespace {

constexpr std::chrono::milliseconds SAVE_DELAY{200};
constexpr std::chrono::milliseconds FIRST_RETRY_DELAY{250};
constexpr std::chrono::milliseconds LAST_RETRY_DELAY{30000};
constexpr const char* LOG_SOURCE = "config";

std::chrono::milliseconds retryDelayAfter(uint32_t failures) {
    std::chrono::milliseconds delay = FIRST_RETRY_DELAY;
    for (uint32_t attempt = 1; attempt < failures && delay < LAST_RETRY_DELAY; ++attempt) {
        delay *= 2;
    }
    return std::min(delay, LAST_RETRY_DELAY);
}

void logSaved(ILogPort* log, uint32_t failuresBefore) {
    if (log == nullptr || failuresBefore == 0) return;
    log->logMsg(LogLevel::INFO, LOG_SOURCE,
                "Saved the configuration after " + std::to_string(failuresBefore) +
                    " failed attempt(s)");
}

void logNotSaved(ILogPort* log, uint32_t failures, bool finalAttempt) {
    if (log == nullptr) return;
    if (finalAttempt) {
        log->logMsg(LogLevel::WARN, LOG_SOURCE, "Could not save the configuration before exiting");
        return;
    }
    if (failures != 1) return;
    log->logMsg(LogLevel::WARN, LOG_SOURCE, "Could not save the configuration; retrying");
}

} // namespace

ConfigStore::ConfigStore(Config initial, Persist persist, ILogPort* log)
    : persist_(std::move(persist)), log_(log),
      snapshot_(std::make_shared<const Config>(std::move(initial))) {
    publishHotFields(*snapshot_);
    saverRunning_ = true;
    saver_ = std::thread(&ConfigStore::saverLoop, this);
}

ConfigStore::~ConfigStore() { stop(); }

std::shared_ptr<const Config> ConfigStore::current() const {
    std::lock_guard<TrackedMutex> lk(snapshotMtx_);
    return snapshot_;
}

std::shared_ptr<const Config> ConfigStore::update(const std::function<bool(Config&)>& mutate) {
    std::lock_guard<TrackedMutex> writer(writeMtx_);
    const std::shared_ptr<const Config> before = snapshot_;
    std::shared_ptr<Config> next = std::make_shared<Config>(*before);
    if (!mutate(*next)) return before;
    const std::shared_ptr<const Config> published = std::move(next);
    {
        std::lock_guard<TrackedMutex> lk(snapshotMtx_);
        snapshot_ = published;
    }
    publishHotFields(*published);
    ++generation_;
    changed_.notify_all();
    return published;
}

ControllerAudioPolicy ConfigStore::audioPolicy() const noexcept {
    return ControllerAudioPolicy{audioMic_.load(std::memory_order_relaxed),
                                 audioSpeaker_.load(std::memory_order_relaxed),
                                 audioHaptics_.load(std::memory_order_relaxed)};
}

int ConfigStore::udpPort() const noexcept { return udpPort_.load(std::memory_order_relaxed); }

bool ConfigStore::flush(std::chrono::milliseconds deadline) {
    std::unique_lock<TrackedMutex> lk(writeMtx_);
    const uint64_t wanted = generation_;
    if (persistedGeneration_ >= wanted) return true;
    const auto settled = [this, wanted] {
        return persistedGeneration_ >= wanted || !saverRunning_;
    };
    ++flushWaiters_;
    changed_.notify_all();
    changed_.wait_for(lk, deadline, settled);
    --flushWaiters_;
    return persistedGeneration_ >= wanted;
}

void ConfigStore::stop() {
    std::thread saver;
    {
        std::lock_guard<TrackedMutex> lk(writeMtx_);
        stopping_ = true;
        saver.swap(saver_);
        changed_.notify_all();
    }
    if (!saver.joinable()) return;
    blockingRegion("thread.join");
    saver.join();
}

void ConfigStore::saverLoop() {
    ScopedThreadRole role(ThreadRole::ConfigSaver);
    std::unique_lock<TrackedMutex> lk(writeMtx_);
    const auto somethingToSave = [this] {
        return stopping_ || generation_ != persistedGeneration_;
    };
    const auto hurried = [this] { return stopping_ || flushWaiters_ > 0; };
    const auto stopped = [this] { return stopping_; };
    uint32_t failures = 0;
    for (;;) {
        changed_.wait(lk, somethingToSave);
        if (generation_ == persistedGeneration_) break;
        if (failures == 0) changed_.wait_for(lk, SAVE_DELAY, hurried);
        const uint64_t saving = generation_;
        const std::shared_ptr<const Config> snapshot = snapshot_;
        const bool finalAttempt = stopping_;
        lk.unlock();
        const bool saved = persist_ ? persist_(*snapshot) : true;
        if (saved) {
            logSaved(log_, failures);
        } else {
            logNotSaved(log_, failures + 1, finalAttempt);
        }
        lk.lock();
        if (saved) {
            persistedGeneration_ = saving;
            failures = 0;
            changed_.notify_all();
            continue;
        }
        ++failures;
        if (stopping_) break;
        changed_.wait_for(lk, retryDelayAfter(failures), stopped);
    }
    saverRunning_ = false;
    changed_.notify_all();
}

void ConfigStore::publishHotFields(const Config& cfg) noexcept {
    audioMic_.store(cfg.controllerAudioMic, std::memory_order_relaxed);
    audioSpeaker_.store(cfg.controllerAudioSpeaker, std::memory_order_relaxed);
    audioHaptics_.store(cfg.controllerAudioHaptics, std::memory_order_relaxed);
    udpPort_.store(cfg.udpPort, std::memory_order_relaxed);
}
