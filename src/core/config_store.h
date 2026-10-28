// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/ports.h"
#include "core/tracked_mutex.h"
#include "core/types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

class ConfigStore {
  public:
    using Persist = std::function<bool(const Config&)>;

    ConfigStore(Config initial, Persist persist, ILogPort* log = nullptr);
    ~ConfigStore();

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    std::shared_ptr<const Config> current() const;
    std::shared_ptr<const Config> update(const std::function<bool(Config&)>& mutate);

    ControllerAudioPolicy audioPolicy() const noexcept;
    int udpPort() const noexcept;

    bool flush(std::chrono::milliseconds deadline);
    void stop();

  private:
    void saverLoop();
    void publishHotFields(const Config& cfg) noexcept;

    const Persist persist_;
    ILogPort* const log_;

    mutable TrackedMutex snapshotMtx_{"config.snapshot"};
    std::shared_ptr<const Config> snapshot_;

    TrackedMutex writeMtx_{"config.write"};
    std::condition_variable_any changed_;
    uint64_t generation_ = 0;
    uint64_t persistedGeneration_ = 0;
    int flushWaiters_ = 0;
    bool stopping_ = false;
    bool saverRunning_ = false;
    std::thread saver_;

    std::atomic<bool> audioMic_{true};
    std::atomic<bool> audioSpeaker_{true};
    std::atomic<bool> audioHaptics_{true};
    std::atomic<int> udpPort_{DEFAULT_UDP_PORT};
};
