// SPDX-License-Identifier: LGPL-3.0-or-later
#include "test_util.h"

#include "core/tracked_mutex.h"
#include "core/types.h"
#include "hidmaestro_helper_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

void logMsg(LogLevel, const std::string&, const std::string&) {}

namespace {

namespace hm = satellite::hidmaestro;

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr DWORD SHORT_WAIT_MS = 300;
constexpr DWORD LONG_WAIT_MS = 5000;
constexpr DWORD EXIT_WAIT_MS = 50;
constexpr DWORD SLOW_MS = 1000;
constexpr DWORD WARM_LIMIT_MS = 10000;
constexpr DWORD REAL_SLOW_MS = 10500;
constexpr auto TIMEOUT_SLACK = milliseconds(4000);

const std::string HELLO_REPLY = R"({"ok":true,"helper":"test","audio":true,"broker":true})";
const std::string OK_REPLY = R"({"ok":true})";
const std::string PLUG_REPLY =
    R"({"ok":true,"index":3,"input":1001,"inputEvent":1002,"companionEvent":0,"output":1003,"outputEvent":1004})";

bool isHello(const std::string& request) { return request.find("\"hello\"") != std::string::npos; }

class FakeHelper {
  public:
    using Handler = std::function<void(FakeHelper&, const std::string&)>;

    explicit FakeHelper(Handler handler) : handler_(std::move(handler)) {
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeDone_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    FakeHelper(const FakeHelper&) = delete;
    FakeHelper& operator=(const FakeHelper&) = delete;

    ~FakeHelper() {
        hangUp();
        if (server_ != INVALID_HANDLE_VALUE) CloseHandle(server_);
        CloseHandle(stop_);
        CloseHandle(writeDone_);
    }

    HANDLE launchAs(const std::wstring& pipeName, DWORD delayMs) {
        thread_ = std::thread([this, pipeName, delayMs] {
            if (WaitForSingleObject(stop_, delayMs) == WAIT_OBJECT_0) return;
            HANDLE client = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (client == INVALID_HANDLE_VALUE) return;
            server_ = client;
            serve();
        });
        HANDLE process = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process,
                        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0);
        return process;
    }

    HANDLE connect() {
        static std::atomic<int> sequence{0};
        const std::wstring name = L"\\\\.\\pipe\\satellite-helper-client-test-" +
                                  std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(sequence.fetch_add(1));
        server_ = CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, PIPE_BUFFER, PIPE_BUFFER, 0,
            nullptr);
        if (server_ == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (client == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        thread_ = std::thread([this] { serve(); });
        return client;
    }

    bool write(const std::string& bytes) {
        OVERLAPPED ov{};
        ov.hEvent = writeDone_;
        ResetEvent(writeDone_);
        DWORD put = 0;
        if (!WriteFile(server_, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &ov) &&
            GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        return GetOverlappedResult(server_, &ov, &put, TRUE) != 0 && put == bytes.size();
    }

    void pause(DWORD ms) { WaitForSingleObject(stop_, ms); }

    void hangUp() {
        SetEvent(stop_);
        if (thread_.joinable()) thread_.join();
    }

    int requests() const { return requests_.load(); }

  private:
    static constexpr DWORD PIPE_BUFFER = 64 * 1024;

    void serve() {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::string pending;
        char buffer[1024];
        for (;;) {
            const size_t end = pending.find('\n');
            if (end != std::string::npos) {
                const std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                requests_++;
                handler_(*this, line);
                continue;
            }
            if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
            ResetEvent(ov.hEvent);
            DWORD got = 0;
            if (!ReadFile(server_, buffer, sizeof(buffer), nullptr, &ov) &&
                GetLastError() != ERROR_IO_PENDING) {
                break;
            }
            HANDLE waits[2] = {ov.hEvent, stop_};
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(server_, &ov);
                GetOverlappedResult(server_, &ov, &got, TRUE);
                break;
            }
            if (!GetOverlappedResult(server_, &ov, &got, FALSE) || got == 0) break;
            pending.append(buffer, got);
        }
        CloseHandle(ov.hEvent);
        DisconnectNamedPipe(server_);
    }

    Handler handler_;
    HANDLE server_ = INVALID_HANDLE_VALUE;
    HANDLE stop_ = nullptr;
    HANDLE writeDone_ = nullptr;
    std::thread thread_;
    std::atomic<int> requests_{0};
};

FakeHelper::Handler answersEverything() {
    return [](FakeHelper& helper, const std::string& request) {
        helper.write((isHello(request) ? HELLO_REPLY : PLUG_REPLY) + "\n");
    };
}

FakeHelper::Handler answersHelloThen(std::function<void(FakeHelper&)> reply) {
    return [reply = std::move(reply)](FakeHelper& helper, const std::string& request) {
        if (isHello(request)) {
            helper.write(HELLO_REPLY + "\n");
        } else {
            reply(helper);
        }
    };
}

FakeHelper::Handler silentAfterHello() {
    return answersHelloThen([](FakeHelper& helper) { helper.pause(60000); });
}

FakeHelper::Handler silentHelper() {
    return [](FakeHelper& helper, const std::string&) { helper.pause(60000); };
}

FakeHelper::Handler slowPlug(DWORD delayMs) {
    return [delayMs](FakeHelper& helper, const std::string& request) {
        if (isHello(request)) {
            helper.write(HELLO_REPLY + "\n");
        } else if (request.find("\"plug\"") != std::string::npos) {
            helper.pause(delayMs);
            helper.write(PLUG_REPLY + "\n");
        } else {
            helper.write(OK_REPLY + "\n");
        }
    };
}

long long elapsedMs(Clock::time_point since) {
    return std::chrono::duration_cast<milliseconds>(Clock::now() - since).count();
}

uint64_t readsSince(uint64_t before) { return hm::HelperClient::pipeReadsForTest() - before; }

struct Transports {
    std::atomic<int> broker{0};
    std::atomic<int> launches{0};
};

using LaunchFn = std::function<HANDLE(const std::wstring&)>;

hm::HelperClient::TestTransports transportsFor(Transports& calls, std::function<HANDLE()> broker,
                                               LaunchFn launch = {}) {
    hm::HelperClient::TestTransports transports;
    transports.broker = [&calls, broker = std::move(broker)]() -> HANDLE {
        calls.broker++;
        return broker ? broker() : INVALID_HANDLE_VALUE;
    };
    transports.launch = [&calls,
                         launch = std::move(launch)](const std::wstring& pipeName) -> HANDLE {
        calls.launches++;
        return launch ? launch(pipeName) : nullptr;
    };
    return transports;
}

} // namespace

static void test_every_pipe_wait_is_bounded_at_ten_seconds() {
    TEST("timeouts: the short bound is ten seconds");
    EXPECT_EQ(hm::HELPER_PIPE_WAIT_MS, static_cast<DWORD>(10000));
}

static void test_each_request_carries_its_own_limit() {
    TEST("limits: hello, unplug and shutdown keep the ten second bound");
    EXPECT_EQ(hm::helperRequestWaitMs(hm::HelperRequest::Hello), static_cast<DWORD>(10000));
    EXPECT_EQ(hm::helperRequestWaitMs(hm::HelperRequest::Deprovision), static_cast<DWORD>(10000));
    EXPECT_EQ(hm::helperRequestWaitMs(hm::HelperRequest::Shutdown), static_cast<DWORD>(10000));

    TEST("limits: a plug gets 120 s, for the transport install inside it");
    EXPECT_EQ(hm::helperRequestWaitMs(hm::HelperRequest::Provision), static_cast<DWORD>(120000));
    EXPECT_EQ(hm::HELPER_PROVISION_WAIT_MS, static_cast<DWORD>(120000));

    TEST("limits: a launched helper gets 120 s to connect, for the person at the prompt");
    EXPECT_EQ(hm::HELPER_LAUNCH_WAIT_MS, static_cast<DWORD>(120000));
}

static void test_a_reply_that_arrives_whole_costs_one_read() {
    TEST("framing: a reply that arrives whole is read with one read");
    FakeHelper fake(answersEverything());
    hm::HelperClient client;
    EXPECT(client.adoptChannelForTest(fake.connect()));
    const uint64_t before = hm::HelperClient::pipeReadsForTest();
    hm::ProvisionResult out;
    EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
    EXPECT_EQ(readsSince(before), static_cast<uint64_t>(1));
    EXPECT_EQ(out.controllerIndex, static_cast<uint32_t>(3));
    EXPECT_EQ(out.inputSection, static_cast<uint64_t>(1001));
    EXPECT_EQ(out.outputEvent, static_cast<uint64_t>(1004));
    client.shutdown();
}

static void test_a_reply_costs_no_more_reads_than_it_arrives_in_pieces() {
    TEST("framing: the line ending that follows its body costs one more read, no more");
    {
        FakeHelper fake(answersHelloThen([](FakeHelper& helper) {
            helper.write(PLUG_REPLY);
            helper.pause(60);
            helper.write("\n");
        }));
        hm::HelperClient client;
        EXPECT(client.adoptChannelForTest(fake.connect()));
        const uint64_t before = hm::HelperClient::pipeReadsForTest();
        hm::ProvisionResult out;
        EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
        EXPECT(readsSince(before) <= 2);
        EXPECT_EQ(out.inputEvent, static_cast<uint64_t>(1002));
        client.shutdown();
    }

    TEST("framing: a reply in four pieces costs at most four reads");
    {
        FakeHelper fake(answersHelloThen([](FakeHelper& helper) {
            const std::string line = PLUG_REPLY + "\n";
            const size_t piece = line.size() / 4;
            for (size_t at = 0; at < line.size(); at += piece) {
                helper.write(line.substr(at, piece));
                helper.pause(40);
            }
        }));
        hm::HelperClient client;
        EXPECT(client.adoptChannelForTest(fake.connect()));
        const uint64_t before = hm::HelperClient::pipeReadsForTest();
        hm::ProvisionResult out;
        EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
        EXPECT(readsSince(before) <= 5);
        EXPECT_EQ(out.inputSection, static_cast<uint64_t>(1001));
        client.shutdown();
    }

    TEST("framing: a line larger than a small buffer still costs a handful of reads");
    {
        const std::string padding(20000, 'x');
        FakeHelper fake(answersHelloThen([&padding](FakeHelper& helper) {
            helper.write(R"({"ok":true,"index":3,"input":1001,"inputEvent":1002,"pad":")" +
                         padding + "\"}\n");
        }));
        hm::HelperClient client;
        EXPECT(client.adoptChannelForTest(fake.connect()));
        const uint64_t before = hm::HelperClient::pipeReadsForTest();
        hm::ProvisionResult out;
        EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
        EXPECT(readsSince(before) <= 2);
        client.shutdown();
    }
}

static void test_a_carriage_return_before_the_line_feed_is_part_of_the_framing() {
    TEST("framing: a CRLF line ending reads the same as a bare line feed");
    FakeHelper fake(
        answersHelloThen([](FakeHelper& helper) { helper.write(PLUG_REPLY + "\r\n"); }));
    hm::HelperClient client;
    EXPECT(client.adoptChannelForTest(fake.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
    EXPECT_EQ(out.inputSection, static_cast<uint64_t>(1001));
    client.shutdown();
}

static void test_a_reply_with_no_end_is_refused_at_the_size_limit() {
    TEST("framing: a reply that never ends is refused at the size limit and drops the channel");
    FakeHelper fake(answersHelloThen([](FakeHelper& helper) {
        helper.write(std::string(70 * 1024, 'x'));
        helper.pause(60000);
    }));
    hm::HelperClient client;
    EXPECT(client.adoptChannelForTest(fake.connect()));
    const auto start = Clock::now();
    hm::ProvisionResult out;
    EXPECT(!client.provision(3, GamepadIdentity::Xbox, false, out));
    EXPECT(elapsedMs(start) < TIMEOUT_SLACK.count());
    EXPECT(!client.isReady());
}

static void test_a_silent_helper_fails_the_call_at_the_deadline() {
    TEST("timeouts: a helper that never answers fails the call at the deadline, not after 120 s");
    FakeHelper fake(silentAfterHello());
    hm::HelperClient client;
    client.setPipeWaitForTest(SHORT_WAIT_MS);
    EXPECT(client.adoptChannelForTest(fake.connect()));
    const auto start = Clock::now();
    EXPECT(!client.deprovision(3));
    const long long took = elapsedMs(start);
    EXPECT(took >= static_cast<long long>(SHORT_WAIT_MS) - 50);
    EXPECT(took < TIMEOUT_SLACK.count());

    TEST("timeouts: the call that timed out drops the channel, so the next plug starts fresh");
    EXPECT(!client.isReady());
}

static void test_a_half_line_is_not_a_reply() {
    TEST("timeouts: half a line and then silence fails the call at the deadline");
    FakeHelper fake(answersHelloThen([](FakeHelper& helper) {
        helper.write(R"({"ok":tr)");
        helper.pause(60000);
    }));
    hm::HelperClient client;
    client.setProvisionWaitForTest(SHORT_WAIT_MS);
    EXPECT(client.adoptChannelForTest(fake.connect()));
    const auto start = Clock::now();
    hm::ProvisionResult out;
    EXPECT(!client.provision(3, GamepadIdentity::Xbox, false, out));
    EXPECT(elapsedMs(start) < TIMEOUT_SLACK.count());
    EXPECT(!client.isReady());
}

static void test_the_deadline_covers_the_whole_reply() {
    TEST("timeouts: a reply trickled in a byte at a time still ends at the one deadline");
    FakeHelper fake(answersHelloThen([](FakeHelper& helper) {
        for (int i = 0; i < 60; ++i) {
            if (!helper.write("x")) break;
            helper.pause(100);
        }
    }));
    hm::HelperClient client;
    client.setProvisionWaitForTest(SHORT_WAIT_MS);
    EXPECT(client.adoptChannelForTest(fake.connect()));
    const auto start = Clock::now();
    hm::ProvisionResult out;
    EXPECT(!client.provision(3, GamepadIdentity::Xbox, false, out));
    const long long took = elapsedMs(start);
    EXPECT(took >= static_cast<long long>(SHORT_WAIT_MS) - 50);
    EXPECT(took < 1500);
    EXPECT(!client.isReady());
}

static void test_a_slow_first_provision_succeeds_within_its_limit() {
    TEST("limits: a plug that runs past the short bound still succeeds within its own limit");
    FakeHelper fake(slowPlug(SLOW_MS));
    hm::HelperClient client;
    client.setPipeWaitForTest(SHORT_WAIT_MS);
    client.setProvisionWaitForTest(LONG_WAIT_MS);
    EXPECT(client.adoptChannelForTest(fake.connect()));
    const auto start = Clock::now();
    hm::ProvisionResult out;
    EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
    const long long took = elapsedMs(start);
    EXPECT(took >= static_cast<long long>(SLOW_MS) - 50);
    EXPECT(took < static_cast<long long>(LONG_WAIT_MS));
    EXPECT_EQ(out.inputSection, static_cast<uint64_t>(1001));
    EXPECT(client.isReady());

    TEST("limits: an unplug beside that long plug limit is still held to the short bound");
    FakeHelper stalling(answersHelloThen([](FakeHelper& helper) { helper.pause(60000); }));
    hm::HelperClient other;
    other.setPipeWaitForTest(SHORT_WAIT_MS);
    other.setProvisionWaitForTest(LONG_WAIT_MS);
    EXPECT(other.adoptChannelForTest(stalling.connect()));
    const auto stalledAt = Clock::now();
    EXPECT(!other.deprovision(3));
    EXPECT(elapsedMs(stalledAt) < TIMEOUT_SLACK.count());
    EXPECT(!other.isReady());
    client.shutdown();
}

static void test_a_plug_that_stalls_still_fails_at_its_limit() {
    TEST("limits: a plug that never answers fails at its own limit, not later");
    FakeHelper fake(silentAfterHello());
    hm::HelperClient client;
    client.setPipeWaitForTest(SHORT_WAIT_MS);
    client.setProvisionWaitForTest(800);
    EXPECT(client.adoptChannelForTest(fake.connect()));
    const auto start = Clock::now();
    hm::ProvisionResult out;
    EXPECT(!client.provision(3, GamepadIdentity::Xbox, false, out));
    const long long took = elapsedMs(start);
    EXPECT(took >= 750);
    EXPECT(took < TIMEOUT_SLACK.count());
    EXPECT(!client.isReady());
}

static void test_hello_and_shutdown_keep_the_short_bound_beside_a_long_plug() {
    TEST("limits: a hello that is never answered fails at the short bound");
    {
        FakeHelper fake(silentHelper());
        hm::HelperClient client;
        client.setPipeWaitForTest(SHORT_WAIT_MS);
        client.setProvisionWaitForTest(LONG_WAIT_MS);
        const auto start = Clock::now();
        EXPECT(!client.adoptChannelForTest(fake.connect()));
        EXPECT(elapsedMs(start) < TIMEOUT_SLACK.count());
        EXPECT(!client.isReady());
    }

    TEST("limits: a shutdown that is never answered gives up at the short bound");
    {
        FakeHelper fake(silentAfterHello());
        hm::HelperClient client;
        client.setPipeWaitForTest(SHORT_WAIT_MS);
        client.setProvisionWaitForTest(LONG_WAIT_MS);
        EXPECT(client.adoptChannelForTest(fake.connect()));
        const auto start = Clock::now();
        client.shutdown();
        EXPECT(elapsedMs(start) < TIMEOUT_SLACK.count());
        EXPECT(!client.isReady());
    }
}

static void test_a_slow_launch_connect_succeeds_within_its_limit() {
    TEST("limits: a launched helper slower than the short bound to connect still connects");
    Transports calls;
    FakeHelper launched(answersEverything());
    hm::HelperClient client;
    client.setPipeWaitForTest(SHORT_WAIT_MS);
    client.setLaunchWaitForTest(LONG_WAIT_MS);
    client.setExitWaitForTest(EXIT_WAIT_MS);
    client.setTransportsForTest(transportsFor(calls, {}, [&launched](const std::wstring& name) {
        return launched.launchAs(name, SLOW_MS);
    }));
    const auto start = Clock::now();
    EXPECT(client.ensureReady());
    const long long took = elapsedMs(start);
    EXPECT(took >= static_cast<long long>(SLOW_MS) - 50);
    EXPECT(took < static_cast<long long>(LONG_WAIT_MS));
    EXPECT_EQ(calls.launches.load(), 1);
    hm::ProvisionResult out;
    EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
    client.shutdown();
}

static void test_a_launch_that_never_connects_fails_at_its_limit() {
    TEST("limits: a launched helper that never connects fails at the launch limit");
    Transports calls;
    FakeHelper launched(answersEverything());
    hm::HelperClient client;
    client.setPipeWaitForTest(SHORT_WAIT_MS);
    client.setLaunchWaitForTest(800);
    client.setExitWaitForTest(EXIT_WAIT_MS);
    client.setTransportsForTest(transportsFor(calls, {}, [&launched](const std::wstring& name) {
        return launched.launchAs(name, 60000);
    }));
    const auto start = Clock::now();
    EXPECT(!client.ensureReady());
    const long long took = elapsedMs(start);
    EXPECT(took >= 750);
    EXPECT(took < TIMEOUT_SLACK.count());
    EXPECT_EQ(calls.launches.load(), 1);
    EXPECT(!client.isReady());
}

static void test_the_shipped_limits_hold_in_real_time() {
    struct Outcome {
        bool ok = false;
        long long tookMs = 0;
    };
    Outcome stalled;
    Outcome plug;
    Outcome launch;

    std::thread stalledCall([&stalled] {
        FakeHelper fake(silentAfterHello());
        hm::HelperClient client;
        client.adoptChannelForTest(fake.connect());
        const auto start = Clock::now();
        stalled.ok = client.deprovision(3);
        stalled.tookMs = elapsedMs(start);
    });
    std::thread slowPlugCall([&plug] {
        FakeHelper fake(slowPlug(REAL_SLOW_MS));
        hm::HelperClient client;
        client.adoptChannelForTest(fake.connect());
        const auto start = Clock::now();
        hm::ProvisionResult out;
        plug.ok = client.provision(3, GamepadIdentity::Xbox, false, out);
        plug.tookMs = elapsedMs(start);
        client.shutdown();
    });
    std::thread slowLaunchCall([&launch] {
        Transports calls;
        FakeHelper launched(answersEverything());
        hm::HelperClient client;
        client.setExitWaitForTest(EXIT_WAIT_MS);
        client.setTransportsForTest(transportsFor(calls, {}, [&launched](const std::wstring& name) {
            return launched.launchAs(name, REAL_SLOW_MS);
        }));
        const auto start = Clock::now();
        launch.ok = client.ensureReady();
        launch.tookMs = elapsedMs(start);
        client.shutdown();
    });
    stalledCall.join();
    slowPlugCall.join();
    slowLaunchCall.join();

    TEST("limits (real time): a stalled unplug fails at ten seconds");
    EXPECT(!stalled.ok);
    EXPECT(stalled.tookMs >= 9500);
    EXPECT(stalled.tookMs < 25000);

    TEST("limits (real time): a plug that takes longer than ten seconds succeeds");
    EXPECT(plug.ok);
    EXPECT(plug.tookMs >= 10000);
    EXPECT(plug.tookMs < 60000);

    TEST("limits (real time): a launched helper that connects after ten seconds succeeds");
    EXPECT(launch.ok);
    EXPECT(launch.tookMs >= 10000);
    EXPECT(launch.tookMs < 60000);
}

static void test_a_connection_made_ahead_serves_the_first_plug() {
    TEST("background connect: the service is connected before any plug, and the plug uses it");
    Transports calls;
    FakeHelper broker(answersEverything());
    hm::HelperClient client;
    client.setTransportsForTest(transportsFor(calls, [&broker] { return broker.connect(); }));
    EXPECT(!client.isReady());
    client.connectBrokerInBackground();
    EXPECT(client.waitForBackgroundConnectForTest(WARM_LIMIT_MS));
    EXPECT(client.isReady());
    EXPECT_EQ(calls.broker.load(), 1);
    EXPECT_EQ(calls.launches.load(), 0);

    TEST("background connect: ensureReady takes the connection made ahead, with no second connect");
    EXPECT(client.ensureReady());
    hm::ProvisionResult out;
    EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
    EXPECT_EQ(out.inputSection, static_cast<uint64_t>(1001));
    EXPECT_EQ(calls.broker.load(), 1);
    EXPECT_EQ(calls.launches.load(), 0);
    client.shutdown();
}

static void test_the_connection_ahead_never_takes_the_elevation_prompt() {
    TEST("background connect: with no service to reach it gives up and never launches a helper");
    Transports calls;
    FakeHelper launched(answersEverything());
    hm::HelperClient client;
    client.setExitWaitForTest(EXIT_WAIT_MS);
    client.setTransportsForTest(transportsFor(
        calls, {}, [&launched](const std::wstring& name) { return launched.launchAs(name, 0); }));
    client.connectBrokerInBackground();
    EXPECT(client.waitForBackgroundConnectForTest(WARM_LIMIT_MS));
    EXPECT(!client.isReady());
    EXPECT_EQ(calls.broker.load(), 1);
    EXPECT_EQ(calls.launches.load(), 0);

    TEST("background connect: the plug that needs a helper still falls back to the launch");
    EXPECT(client.ensureReady());
    EXPECT_EQ(calls.launches.load(), 1);
    EXPECT(client.isReady());
    client.shutdown();
}

static void test_a_plug_that_arrives_mid_connect_waits_for_it() {
    TEST("background connect: a plug that lands mid-connect waits for it instead of connecting "
         "twice");
    Transports calls;
    FakeHelper broker(answersEverything());
    hm::HelperClient client;
    client.setTransportsForTest(transportsFor(calls, [&broker] {
        Sleep(300);
        return broker.connect();
    }));
    client.connectBrokerInBackground();
    const auto start = Clock::now();
    EXPECT(client.ensureReady());
    EXPECT(elapsedMs(start) >= 200);
    EXPECT_EQ(calls.broker.load(), 1);
    EXPECT_EQ(calls.launches.load(), 0);
    client.shutdown();
}

static void test_one_connect_at_a_time_and_none_when_a_channel_is_up() {
    TEST("background connect: a second request while one is running starts nothing");
    {
        Transports calls;
        FakeHelper broker(answersEverything());
        hm::HelperClient client;
        client.setTransportsForTest(transportsFor(calls, [&broker] {
            Sleep(200);
            return broker.connect();
        }));
        client.connectBrokerInBackground();
        client.connectBrokerInBackground();
        EXPECT(client.waitForBackgroundConnectForTest(WARM_LIMIT_MS));
        EXPECT_EQ(calls.broker.load(), 1);
        client.connectBrokerInBackground();
        EXPECT(client.waitForBackgroundConnectForTest(WARM_LIMIT_MS));
        EXPECT_EQ(calls.broker.load(), 1);
        client.shutdown();
    }

    TEST("background connect: nothing is started while a channel is already up");
    {
        Transports calls;
        FakeHelper adopted(answersEverything());
        hm::HelperClient client;
        client.setTransportsForTest(transportsFor(calls, {}));
        EXPECT(client.adoptChannelForTest(adopted.connect()));
        client.connectBrokerInBackground();
        EXPECT(client.waitForBackgroundConnectForTest(WARM_LIMIT_MS));
        EXPECT_EQ(calls.broker.load(), 0);
        client.shutdown();
    }
}

static void test_a_connection_that_died_while_idle_is_not_used() {
    TEST(
        "background connect: a connection that died while idle is dropped and the plug reconnects");
    Transports calls;
    FakeHelper first(answersEverything());
    FakeHelper second(answersEverything());
    hm::HelperClient client;
    client.setTransportsForTest(transportsFor(
        calls, [&] { return calls.broker.load() == 1 ? first.connect() : second.connect(); }));
    client.connectBrokerInBackground();
    EXPECT(client.waitForBackgroundConnectForTest(WARM_LIMIT_MS));
    EXPECT(client.isReady());
    first.hangUp();
    EXPECT(!client.isReady());
    EXPECT(client.ensureReady());
    EXPECT_EQ(calls.broker.load(), 2);
    EXPECT_EQ(calls.launches.load(), 0);
    hm::ProvisionResult out;
    EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
    client.shutdown();
}

static void test_shutdown_does_not_wait_out_a_connect_in_flight() {
    TEST("background connect: shutdown cancels a connect that is waiting on the service");
    Transports calls;
    FakeHelper silent(silentHelper());
    hm::HelperClient client;
    client.setTransportsForTest(transportsFor(calls, [&silent] { return silent.connect(); }));
    client.connectBrokerInBackground();
    Sleep(150);
    const auto start = Clock::now();
    client.shutdown();
    EXPECT(elapsedMs(start) < 3000);
    EXPECT(!client.isReady());
    EXPECT(client.waitForBackgroundConnectForTest(0));
}

static void test_a_driver_install_starts_the_connect() {
    TEST("background connect: a finished driver install starts the connect for the client");
    Transports calls;
    FakeHelper broker(answersEverything());
    hm::HelperClient client;
    client.setTransportsForTest(transportsFor(calls, [&broker] { return broker.connect(); }));
    hm::connectBrokerWhenDriverInstalled(&client);
    hm::notifyDriverInstalledForTest();
    EXPECT(client.waitForBackgroundConnectForTest(WARM_LIMIT_MS));
    EXPECT(client.isReady());
    hm::connectBrokerWhenDriverInstalled(nullptr);
    client.shutdown();

    TEST("background connect: a client that was never registered is left alone");
    Transports other;
    hm::HelperClient bystander;
    bystander.setTransportsForTest(transportsFor(other, {}));
    hm::notifyDriverInstalledForTest();
    EXPECT(bystander.waitForBackgroundConnectForTest(WARM_LIMIT_MS));
    EXPECT_EQ(other.broker.load(), 0);
}

#ifdef SATELLITE_LOCK_STATS
static void test_the_connect_ahead_takes_no_lock() {
    TEST("locking: the connect made ahead takes no lock, the plug path takes the helper lock");
    lockstats::reset();
    Transports calls;
    FakeHelper broker(answersEverything());
    hm::HelperClient client;
    client.setTransportsForTest(transportsFor(calls, [&broker] { return broker.connect(); }));
    client.connectBrokerInBackground();
    EXPECT(client.waitForBackgroundConnectForTest(WARM_LIMIT_MS));
    EXPECT(client.ensureReady());
    hm::ProvisionResult out;
    EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
    client.shutdown();

    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::LockStats* helper = snap.find("hidmaestro.helper");
    EXPECT(helper != nullptr);
    if (helper == nullptr) return;
    EXPECT(helper->role(ThreadRole::Other).touched());
    for (size_t r = 0; r < THREAD_ROLE_COUNT; ++r) {
        const ThreadRole role = static_cast<ThreadRole>(r);
        if (role == ThreadRole::Other) continue;
        if (helper->role(role).touched()) {
            std::cerr << "  the " << threadRoleName(role) << " role took hidmaestro.helper\n";
            EXPECT(false);
        }
    }
}
#endif

int main() {
    test_every_pipe_wait_is_bounded_at_ten_seconds();
    test_each_request_carries_its_own_limit();
    test_a_reply_that_arrives_whole_costs_one_read();
    test_a_reply_costs_no_more_reads_than_it_arrives_in_pieces();
    test_a_carriage_return_before_the_line_feed_is_part_of_the_framing();
    test_a_reply_with_no_end_is_refused_at_the_size_limit();
    test_a_silent_helper_fails_the_call_at_the_deadline();
    test_a_half_line_is_not_a_reply();
    test_the_deadline_covers_the_whole_reply();
    test_a_slow_first_provision_succeeds_within_its_limit();
    test_a_plug_that_stalls_still_fails_at_its_limit();
    test_hello_and_shutdown_keep_the_short_bound_beside_a_long_plug();
    test_a_slow_launch_connect_succeeds_within_its_limit();
    test_a_launch_that_never_connects_fails_at_its_limit();
    test_a_connection_made_ahead_serves_the_first_plug();
    test_the_connection_ahead_never_takes_the_elevation_prompt();
    test_a_plug_that_arrives_mid_connect_waits_for_it();
    test_one_connect_at_a_time_and_none_when_a_channel_is_up();
    test_a_connection_that_died_while_idle_is_not_used();
    test_shutdown_does_not_wait_out_a_connect_in_flight();
    test_a_driver_install_starts_the_connect();
#ifdef SATELLITE_LOCK_STATS
    test_the_connect_ahead_takes_no_lock();
#endif
    test_the_shipped_limits_hold_in_real_time();

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
