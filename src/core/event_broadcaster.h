// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/tracked_mutex.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace satellite {

struct EventBroadcasterOptions {
    size_t maxSubscribers = 8;
    std::chrono::milliseconds period{1000};
    bool timed = true;
};

class EventBroadcaster {
    struct Shared;

  public:
    using Build = std::function<std::string()>;
    using Frame = std::shared_ptr<const std::string>;

    class Subscription {
      public:
        ~Subscription();
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Frame next();

      private:
        friend class EventBroadcaster;
        explicit Subscription(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}

        std::shared_ptr<Shared> shared_;
        uint64_t seen_ = 0;
    };

    explicit EventBroadcaster(Build build,
                              EventBroadcasterOptions options = EventBroadcasterOptions());
    ~EventBroadcaster();

    EventBroadcaster(const EventBroadcaster&) = delete;
    EventBroadcaster& operator=(const EventBroadcaster&) = delete;

    std::unique_ptr<Subscription> subscribe();
    bool tick();
    void close();

  private:
    void run();

    const Build build_;
    const std::chrono::milliseconds period_;
    const bool timed_;
    const std::shared_ptr<Shared> shared_;
    std::thread thread_;
};

} // namespace satellite
