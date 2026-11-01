// SPDX-License-Identifier: LGPL-3.0-or-later
#include "net/event_stream.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_util.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using satellite::EventBroadcaster;
using satellite::EventBroadcasterOptions;

constexpr const char* STREAM_PATH = "/events";
constexpr size_t STREAM_CAP = 8;
constexpr int STREAM_COUNT = 8;
constexpr int FRAMES_WANTED = 10;
constexpr auto TICK_PERIOD = 20ms;
constexpr auto WAIT_LIMIT = 10s;
constexpr auto STOP_LIMIT = 3s;
constexpr int CLIENT_TIMEOUT_SEC = 10;

size_t countFrames(const std::string& body) {
    size_t frames = 0;
    for (size_t at = body.find("\n\n"); at != std::string::npos; at = body.find("\n\n", at + 2)) {
        ++frames;
    }
    return frames;
}

template <typename Pred> bool waitFor(Pred pred, std::chrono::milliseconds limit = WAIT_LIMIT) {
    const Clock::time_point deadline = Clock::now() + limit;
    while (!pred()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

class Viewer {
  public:
    Viewer(int port, std::string path)
        : thread_([this, port, path = std::move(path)] { run(port, path); }) {}

    ~Viewer() {
        hangUp();
        thread_.join();
    }

    Viewer(const Viewer&) = delete;
    Viewer& operator=(const Viewer&) = delete;

    void hangUp() { keepReading_.store(false); }
    int status() const { return status_.load(); }
    bool finished() const { return finished_.load(); }

    std::string body() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return body_;
    }

    std::string header(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto it = headers_.find(name);
        return it == headers_.end() ? std::string() : it->second;
    }

    size_t frames() const { return countFrames(body()); }

  private:
    void run(int port, const std::string& path) {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(CLIENT_TIMEOUT_SEC, 0);
        client.set_read_timeout(CLIENT_TIMEOUT_SEC, 0);
        client.Get(
            path,
            [this](const httplib::Response& response) {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    headers_["Content-Type"] = response.get_header_value("Content-Type");
                    headers_["Cache-Control"] = response.get_header_value("Cache-Control");
                }
                status_.store(response.status);
                return true;
            },
            [this](const char* data, size_t length) {
                std::lock_guard<std::mutex> lk(mtx_);
                body_.append(data, length);
                return keepReading_.load();
            });
        finished_.store(true);
    }

    mutable std::mutex mtx_;
    std::string body_;
    std::map<std::string, std::string> headers_;
    std::atomic<bool> keepReading_{true};
    std::atomic<int> status_{0};
    std::atomic<bool> finished_{false};
    std::thread thread_;
};

class Rig {
  public:
    Rig()
        : streams_(std::make_shared<EventBroadcaster>(
              [this] { return "event: tick\ndata: " + std::to_string(++builds_) + "\n\n"; },
              EventBroadcasterOptions{STREAM_CAP, TICK_PERIOD, true})) {
        satellite::serveEventStreams(server_, STREAM_PATH, streams_);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) return;
        serving_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~Rig() { stop(); }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    bool up() const { return port_ > 0; }
    int port() const { return port_; }
    int builds() const { return builds_.load(); }

    bool stop() {
        if (!serving_.joinable()) return true;
        server_.stop();
        const Clock::time_point stopping = Clock::now();
        serving_.join();
        return Clock::now() - stopping < STOP_LIMIT;
    }

  private:
    std::atomic<int> builds_{0};
    std::shared_ptr<EventBroadcaster> streams_;
    httplib::Server server_;
    std::thread serving_;
    int port_ = 0;
};

void testAStreamIsServedAsAnEventStream() {
    TEST("a stream is served as an event stream whose frames are the broadcaster's frames");
    Rig rig;
    EXPECT(rig.up());
    if (!rig.up()) return;
    Viewer viewer(rig.port(), STREAM_PATH);
    EXPECT(waitFor([&] { return viewer.frames() >= 3; }));
    EXPECT_EQ(viewer.status(), 200);
    EXPECT_EQ(viewer.header("Content-Type"), std::string("text/event-stream"));
    EXPECT_EQ(viewer.header("Cache-Control"), std::string("no-cache"));
    EXPECT(viewer.body().rfind("event: tick\ndata: 1\n\n", 0) == 0);
}

void testEveryStreamSharesTheBuilds() {
    TEST("eight streams share the frames: the tick is built once, not once per stream");
    Rig rig;
    EXPECT(rig.up());
    if (!rig.up()) return;
    std::vector<std::unique_ptr<Viewer>> viewers;
    for (int i = 0; i < STREAM_COUNT; ++i) {
        viewers.push_back(std::make_unique<Viewer>(rig.port(), STREAM_PATH));
    }
    const bool allSeen = waitFor([&] {
        for (const std::unique_ptr<Viewer>& viewer : viewers) {
            if (viewer->frames() < FRAMES_WANTED) return false;
        }
        return true;
    });
    EXPECT(allSeen);
    size_t delivered = 0;
    for (const std::unique_ptr<Viewer>& viewer : viewers) {
        EXPECT_EQ(viewer->status(), 200);
        delivered += viewer->frames();
    }
    EXPECT(static_cast<size_t>(rig.builds()) * 2 < delivered);
}

void testTheNinthStreamIsRefusedUntilOneLeaves() {
    TEST("a ninth concurrent stream gets 503, and is served again once a stream has left");
    Rig rig;
    EXPECT(rig.up());
    if (!rig.up()) return;
    std::vector<std::unique_ptr<Viewer>> viewers;
    for (int i = 0; i < STREAM_COUNT; ++i) {
        viewers.push_back(std::make_unique<Viewer>(rig.port(), STREAM_PATH));
    }
    const bool allUp = waitFor([&] {
        for (const std::unique_ptr<Viewer>& viewer : viewers) {
            if (viewer->status() != 200) return false;
        }
        return true;
    });
    EXPECT(allUp);

    httplib::Client client("127.0.0.1", rig.port());
    client.set_read_timeout(CLIENT_TIMEOUT_SEC, 0);
    const httplib::Result refused = client.Get(STREAM_PATH);
    EXPECT(static_cast<bool>(refused));
    if (refused) {
        EXPECT_EQ(refused->status, 503);
        EXPECT(refused->body.find("too many event streams") != std::string::npos);
        EXPECT_EQ(refused->get_header_value("Content-Type"), std::string("application/json"));
    }

    viewers.front()->hangUp();
    std::unique_ptr<Viewer> replacement;
    const bool served = waitFor([&] {
        replacement = std::make_unique<Viewer>(rig.port(), STREAM_PATH);
        const bool answered = waitFor([&] { return replacement->status() != 0; }, 1s);
        if (answered && replacement->status() == 200) return true;
        replacement.reset();
        return false;
    });
    EXPECT(served);
}

void testStoppingTheServerEndsTheOpenStreams() {
    TEST("stopping the server ends the open streams and lets the listener return at once");
    Rig rig;
    EXPECT(rig.up());
    if (!rig.up()) return;
    std::vector<std::unique_ptr<Viewer>> viewers;
    for (int i = 0; i < STREAM_COUNT; ++i) {
        viewers.push_back(std::make_unique<Viewer>(rig.port(), STREAM_PATH));
    }
    EXPECT(waitFor([&] {
        for (const std::unique_ptr<Viewer>& viewer : viewers) {
            if (viewer->frames() < 1) return false;
        }
        return true;
    }));
    EXPECT(rig.stop());
    EXPECT(waitFor(
        [&] {
            for (const std::unique_ptr<Viewer>& viewer : viewers) {
                if (!viewer->finished()) return false;
            }
            return true;
        },
        std::chrono::duration_cast<std::chrono::milliseconds>(STOP_LIMIT)));
}

} // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif
    testAStreamIsServedAsAnEventStream();
    testEveryStreamSharesTheBuilds();
    testTheNinthStreamIsRefusedUntilOneLeaves();
    testStoppingTheServerEndsTheOpenStreams();
#ifdef _WIN32
    WSACleanup();
#endif

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
