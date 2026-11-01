// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/event_broadcaster.h"

#include <condition_variable>
#include <mutex>

namespace satellite {

struct EventBroadcaster::Shared {
    explicit Shared(size_t cap) : maxSubscribers(cap) {}

    const size_t maxSubscribers;
    TrackedMutex mtx{"events.broadcast"};
    std::condition_variable_any published;
    std::condition_variable_any wake;
    Frame latest;
    uint64_t generation = 0;
    size_t subscribers = 0;
    bool freshWanted = false;
    bool closed = false;
};

EventBroadcaster::EventBroadcaster(Build build, EventBroadcasterOptions options)
    : build_(std::move(build)), period_(options.period), timed_(options.timed),
      shared_(std::make_shared<Shared>(options.maxSubscribers)) {}

EventBroadcaster::~EventBroadcaster() { close(); }

std::unique_ptr<EventBroadcaster::Subscription> EventBroadcaster::subscribe() {
    {
        std::lock_guard<TrackedMutex> lk(shared_->mtx);
        if (shared_->closed || shared_->subscribers >= shared_->maxSubscribers) return nullptr;
        if (shared_->subscribers == 0) {
            shared_->latest.reset();
            shared_->freshWanted = true;
        }
        ++shared_->subscribers;
        if (timed_ && !thread_.joinable()) thread_ = std::thread(&EventBroadcaster::run, this);
    }
    shared_->wake.notify_all();
    return std::unique_ptr<Subscription>(new Subscription(shared_));
}

bool EventBroadcaster::tick() {
    {
        std::lock_guard<TrackedMutex> lk(shared_->mtx);
        if (shared_->closed || shared_->subscribers == 0) return false;
    }
    Frame frame = std::make_shared<const std::string>(build_());
    {
        std::lock_guard<TrackedMutex> lk(shared_->mtx);
        if (shared_->closed) return false;
        shared_->latest = std::move(frame);
        ++shared_->generation;
    }
    shared_->published.notify_all();
    return true;
}

void EventBroadcaster::close() {
    std::thread worker;
    {
        std::lock_guard<TrackedMutex> lk(shared_->mtx);
        shared_->closed = true;
        worker.swap(thread_);
    }
    shared_->published.notify_all();
    shared_->wake.notify_all();
    if (!worker.joinable()) return;
    blockingRegion("thread.join");
    worker.join();
}

void EventBroadcaster::run() {
    ScopedThreadRole role(ThreadRole::Background);
    Shared& shared = *shared_;
    std::unique_lock<TrackedMutex> lk(shared.mtx);
    for (;;) {
        shared.wake.wait(lk, [&shared] { return shared.closed || shared.subscribers > 0; });
        if (shared.closed) return;
        const std::chrono::steady_clock::time_point next =
            std::chrono::steady_clock::now() + period_;
        shared.freshWanted = false;
        lk.unlock();
        tick();
        lk.lock();
        shared.wake.wait_until(lk, next, [&shared] {
            return shared.closed || shared.subscribers == 0 || shared.freshWanted;
        });
        if (shared.closed) return;
    }
}

EventBroadcaster::Subscription::~Subscription() {
    {
        std::lock_guard<TrackedMutex> lk(shared_->mtx);
        --shared_->subscribers;
    }
    shared_->wake.notify_all();
}

EventBroadcaster::Frame EventBroadcaster::Subscription::next() {
    Shared& shared = *shared_;
    std::unique_lock<TrackedMutex> lk(shared.mtx);
    shared.published.wait(lk, [this, &shared] {
        return shared.closed || (shared.latest && shared.generation > seen_);
    });
    if (shared.closed) return nullptr;
    seen_ = shared.generation;
    return shared.latest;
}

} // namespace satellite
