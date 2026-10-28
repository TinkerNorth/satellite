// SPDX-License-Identifier: LGPL-3.0-or-later
#include "event_stream.h"

#include <functional>
#include <utility>

namespace satellite {

namespace {

constexpr const char* EVENT_STREAM_MIME = "text/event-stream";
constexpr const char* ERROR_MIME = "application/json";
constexpr const char* TOO_MANY_STREAMS = R"({"error":"too many event streams"})";

class StreamClosingQueue final : public httplib::TaskQueue {
  public:
    explicit StreamClosingQueue(std::shared_ptr<EventBroadcaster> streams)
        : streams_(std::move(streams)),
          pool_(CPPHTTPLIB_THREAD_POOL_COUNT, CPPHTTPLIB_THREAD_POOL_MAX_COUNT) {}

    bool enqueue(std::function<void()> fn) override { return pool_.enqueue(std::move(fn)); }

    void shutdown() override {
        streams_->close();
        pool_.shutdown();
    }

  private:
    std::shared_ptr<EventBroadcaster> streams_;
    httplib::ThreadPool pool_;
};

bool relayFrames(EventBroadcaster::Subscription& stream, httplib::DataSink& sink) {
    while (const EventBroadcaster::Frame frame = stream.next()) {
        if (!sink.write(frame->data(), frame->size())) return false;
    }
    return false;
}

} // namespace

void serveEventStreams(httplib::Server& server, const std::string& path,
                       std::shared_ptr<EventBroadcaster> streams) {
    server.new_task_queue = [streams] { return new StreamClosingQueue(streams); };
    server.Get(path, [streams](const httplib::Request&, httplib::Response& res) {
        const std::shared_ptr<EventBroadcaster::Subscription> stream = streams->subscribe();
        if (!stream) {
            res.status = 503;
            res.set_content(TOO_MANY_STREAMS, ERROR_MIME);
            return;
        }
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            EVENT_STREAM_MIME,
            [stream](size_t, httplib::DataSink& sink) { return relayFrames(*stream, sink); });
    });
}

} // namespace satellite
