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
#include <mutex>
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
    R"({"ok":true,"index":3,"transportIndex":7,"input":1001,"inputEvent":1002,"companionEvent":0,"output":1003,"outputEvent":1004})";
const std::string PLUG_REPLY_WITHOUT_TRANSPORT_INDEX =
    R"({"ok":true,"index":3,"input":1001,"inputEvent":1002,"companionEvent":0,"output":1003,"outputEvent":1004})";
const std::string PLUG_REPLY_WITH_VERSION =
    R"({"ok":true,"index":3,"transportIndex":7,"input":1001,"inputEvent":1002,"companionEvent":0,"output":1003,"outputEvent":1004,"version":1005,"identityTag":17467649568734746738,"usbip":true,"companion":true,"timeoutScale":2.5})";

const std::string HELLO_REPLY_WATCHING =
    R"({"ok":true,"helper":"test","audio":true,"broker":true,"protocol":2})";
const std::string PLUG_REPLY_WITH_WATCH =
    R"({"ok":true,"index":3,"transportIndex":7,"input":1001,"inputEvent":1002,"companionEvent":0,"output":1003,"outputEvent":1004,"version":1005,"identityTag":1,"usbip":true,"watch":1006})";

bool isHello(const std::string& request) { return request.find("\"hello\"") != std::string::npos; }

// The hello a fake helper was sent, written on its serving thread.
class HeardHello {
  public:
    void hear(const std::string& request) {
        std::lock_guard<std::mutex> lk(m_);
        hello_ = request;
    }

    std::string hello() const {
        std::lock_guard<std::mutex> lk(m_);
        return hello_;
    }

  private:
    mutable std::mutex m_;
    std::string hello_;
};

// A section whose one handle the test hands to the client in a reply, as the
// helper's duplicates arrive; alive() says whether any handle still holds it.
class NamedSection {
  public:
    explicit NamedSection(const std::wstring& role)
        : name_(L"Local\\satellite-helper-client-test-" + role + L"-" +
                std::to_wstring(GetCurrentProcessId())),
          handle_(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64,
                                     name_.c_str())) {}
    NamedSection(const NamedSection&) = delete;
    NamedSection& operator=(const NamedSection&) = delete;

    int64_t handleValue() const {
        return static_cast<int64_t>(reinterpret_cast<uintptr_t>(handle_));
    }

    bool alive() const {
        HANDLE opened = OpenFileMappingW(FILE_MAP_READ, FALSE, name_.c_str());
        if (opened == nullptr) return false;
        CloseHandle(opened);
        return true;
    }

  private:
    std::wstring name_;
    HANDLE handle_;
};

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
    // The client closed its end, which ends a real helper's session and every
    // controller in it.
    bool closedByClient() const { return closedByClient_.load(); }

    // Waits until every request so far has been answered and the helper reads
    // again, or has found its pipe closed.
    bool idleWithin(DWORD ms) {
        const auto until = Clock::now() + milliseconds(ms);
        while (Clock::now() < until) {
            const bool answered = handled_.load() == requests_.load();
            if (answered && (reading_.load() || closedByClient_.load())) return true;
            std::this_thread::sleep_for(milliseconds(5));
        }
        return false;
    }

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
                handled_++;
                continue;
            }
            if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
            ResetEvent(ov.hEvent);
            DWORD got = 0;
            if (!ReadFile(server_, buffer, sizeof(buffer), nullptr, &ov) &&
                GetLastError() != ERROR_IO_PENDING) {
                closedByClient_ = true;
                break;
            }
            HANDLE waits[2] = {ov.hEvent, stop_};
            reading_ = true;
            const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            reading_ = false;
            if (woke != WAIT_OBJECT_0) {
                CancelIoEx(server_, &ov);
                GetOverlappedResult(server_, &ov, &got, TRUE);
                break;
            }
            if (!GetOverlappedResult(server_, &ov, &got, FALSE) || got == 0) {
                closedByClient_ = true;
                break;
            }
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
    std::atomic<bool> closedByClient_{false};
    std::atomic<int> handled_{0};
    std::atomic<bool> reading_{false};
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

// A helper on the device watch's protocol: it says so at hello, then answers a
// plug with `reply`.
FakeHelper::Handler watchingHelper(std::string reply, HeardHello* heard = nullptr) {
    return [reply = std::move(reply), heard](FakeHelper& helper, const std::string& request) {
        if (!isHello(request)) {
            helper.write(reply + "\n");
            return;
        }
        if (heard != nullptr) heard->hear(request);
        helper.write(HELLO_REPLY_WATCHING + "\n");
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

// A helper on the watch's protocol whose removal of a pad takes `delayMs`
// before it answers, as one did whose SDK removal Windows made wait.
FakeHelper::Handler slowUnplug(DWORD delayMs) {
    return [delayMs](FakeHelper& helper, const std::string& request) {
        if (isHello(request)) {
            helper.write(HELLO_REPLY_WATCHING + "\n");
        } else if (request.find("\"unplug\"") != std::string::npos) {
            helper.pause(delayMs);
            helper.write(OK_REPLY + "\n");
        } else {
            helper.write(PLUG_REPLY_WITH_WATCH + "\n");
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
    TEST("limits: hello and unplug keep the ten second bound");
    EXPECT_EQ(hm::helperRequestWaitMs(hm::HelperRequest::Hello), static_cast<DWORD>(10000));
    EXPECT_EQ(hm::helperRequestWaitMs(hm::HelperRequest::Deprovision), static_cast<DWORD>(10000));

    TEST("limits: a shutdown gets 120 s, for the helper to finish removing its controllers: 16 "
         "pads at about 7.4 s each against a program that holds them open, should Windows take "
         "them one at a time");
    EXPECT_EQ(hm::helperRequestWaitMs(hm::HelperRequest::Shutdown), static_cast<DWORD>(120000));
    EXPECT_EQ(hm::HELPER_SHUTDOWN_WAIT_MS, static_cast<DWORD>(120000));

    TEST("limits: an answer owed to a lost pad's unplug gets 2 s ahead of the next request, "
         "whatever that request's own bound");
    EXPECT_EQ(hm::HELPER_OWED_WAIT_MS, static_cast<DWORD>(2000));

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
    EXPECT_EQ(out.transportIndex, static_cast<uint32_t>(7));
    EXPECT_EQ(out.inputSection, static_cast<uint64_t>(1001));
    EXPECT_EQ(out.outputEvent, static_cast<uint64_t>(1004));
    client.shutdown();
}

static void test_a_reply_names_where_the_frames_go() {
    TEST("plug reply: a helper that names no transport index is read as the slot's");
    {
        FakeHelper fake(answersHelloThen(
            [](FakeHelper& helper) { helper.write(PLUG_REPLY_WITHOUT_TRANSPORT_INDEX + "\n"); }));
        hm::HelperClient client;
        EXPECT(client.adoptChannelForTest(fake.connect()));
        hm::ProvisionResult out;
        EXPECT(client.provision(3, GamepadIdentity::DualSense, false, out));
        EXPECT_EQ(out.controllerIndex, static_cast<uint32_t>(3));
        EXPECT_EQ(out.transportIndex, static_cast<uint32_t>(3));
        client.shutdown();
    }

    TEST("plug reply: no frame section or doorbell means no plug, and every handle the helper "
         "duplicated for it is closed");
    {
        const NamedSection output(L"output");
        const NamedSection version(L"version");
        const std::string reply = R"({"ok":true,"index":3,"transportIndex":3,"output":)" +
                                  std::to_string(output.handleValue()) + R"(,"version":)" +
                                  std::to_string(version.handleValue()) + "}";
        FakeHelper fake(
            answersHelloThen([reply](FakeHelper& helper) { helper.write(reply + "\n"); }));
        hm::HelperClient client;
        EXPECT(client.adoptChannelForTest(fake.connect()));
        hm::ProvisionResult out;
        EXPECT(!client.provision(3, GamepadIdentity::SwitchPro, false, out));
        EXPECT_EQ(out.inputSection, static_cast<uint64_t>(0));
        EXPECT(!output.alive());
        EXPECT(!version.alive());
        client.shutdown();
    }
}

static void test_a_reply_hands_over_the_version_section() {
    TEST("plug reply: the version section, an identity tag past INT64_MAX and the USB/IP flag "
         "reach the result");
    {
        FakeHelper fake(answersHelloThen(
            [](FakeHelper& helper) { helper.write(PLUG_REPLY_WITH_VERSION + "\n"); }));
        hm::HelperClient client;
        EXPECT(client.adoptChannelForTest(fake.connect()));
        hm::ProvisionResult out;
        EXPECT(client.provision(3, GamepadIdentity::DualSense, true, out));
        EXPECT_EQ(out.versionSection, static_cast<uint64_t>(1005));
        EXPECT_EQ(out.identityTag, 0xF2698EBC35709872ULL);
        EXPECT(out.usbip);
        EXPECT(out.versionReported);
        EXPECT(out.companion);
        EXPECT_EQ(out.timeoutScale, 2.5);
        client.shutdown();
    }

    TEST("plug reply: a reply without them reads as no section, no tag and the driver's path, "
         "which the adapter then refuses");
    {
        FakeHelper fake(answersEverything());
        hm::HelperClient client;
        EXPECT(client.adoptChannelForTest(fake.connect()));
        hm::ProvisionResult out;
        EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
        EXPECT_EQ(out.versionSection, static_cast<uint64_t>(0));
        EXPECT_EQ(out.identityTag, static_cast<uint64_t>(0));
        EXPECT(!out.usbip);
        EXPECT(!out.versionReported);
        EXPECT(!out.companion);
        EXPECT_EQ(out.timeoutScale, 1.0);
        client.shutdown();
    }
}

static void test_the_hello_offers_the_device_watch() {
    TEST("hello: satellite offers the device watch's protocol");
    HeardHello heard;
    FakeHelper fake(watchingHelper(OK_REPLY, &heard));
    hm::HelperClient client;
    EXPECT(client.adoptChannelForTest(fake.connect()));
    EXPECT(heard.hello().find("\"protocol\":2") != std::string::npos);
    client.shutdown();
}

static void test_a_plug_on_the_watch_protocol_hands_over_its_block() {
    TEST("plug reply: on a session the helper settled on the watch's protocol, the plug's watch "
         "block reaches the result");
    {
        FakeHelper fake(watchingHelper(PLUG_REPLY_WITH_WATCH));
        hm::HelperClient client;
        EXPECT(client.adoptChannelForTest(fake.connect()));
        hm::ProvisionResult out;
        EXPECT(client.provision(3, GamepadIdentity::DualSense, true, out));
        EXPECT_EQ(out.watchSection, static_cast<uint64_t>(1006));
        EXPECT(out.watchReported);
        client.shutdown();
    }

    TEST("plug reply: a plug on that session that names no block reports none");
    {
        FakeHelper fake(watchingHelper(PLUG_REPLY));
        hm::HelperClient client;
        EXPECT(client.adoptChannelForTest(fake.connect()));
        hm::ProvisionResult out;
        EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
        EXPECT_EQ(out.watchSection, static_cast<uint64_t>(0));
        EXPECT(!out.watchReported);
        client.shutdown();
    }
}

static void test_a_lost_pads_unplug_answered_at_once_is_confirmed() {
    TEST("teardown: a lost pad's unplug that its helper answers at once is confirmed, and settles "
         "nothing");
    FakeHelper fake(watchingHelper(PLUG_REPLY_WITH_WATCH));
    hm::HelperClient client;
    EXPECT(client.adoptChannelForTest(fake.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(2, GamepadIdentity::DualSense, false, out));
    EXPECT(client.deprovision(2, hm::Teardown::DeviceLost));
    EXPECT(client.isReady());
    EXPECT(client.takeSettledSerials().empty());
    client.shutdown();
}

static void test_a_second_lost_unplug_does_not_wait_behind_the_first() {
    TEST("teardown: while a lost pad's answer is still owed, the next lost pad's unplug is sent "
         "without waiting, and both late answers settle their serials");
    FakeHelper fake(slowUnplug(SHORT_WAIT_MS));
    hm::HelperClient client;
    EXPECT(client.adoptChannelForTest(fake.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(2, GamepadIdentity::DualSense, false, out));
    EXPECT(client.provision(4, GamepadIdentity::DualSense, false, out));
    EXPECT(!client.deprovision(2, hm::Teardown::DeviceLost));
    const auto start = Clock::now();
    EXPECT(!client.deprovision(4, hm::Teardown::DeviceLost));
    EXPECT(elapsedMs(start) < static_cast<long long>(hm::HELPER_LOST_UNPLUG_WAIT_MS) + 50);
    EXPECT(client.isReady());
    EXPECT(client.provision(5, GamepadIdentity::DualSense, false, out));
    EXPECT(client.takeSettledSerials() == std::vector<uint32_t>({2, 4}));
    EXPECT(!fake.closedByClient());
    client.shutdown();
}

static void test_a_pad_whose_helper_stopped_is_let_go_without_asking_it() {
    TEST(
        "teardown: a pad whose helper stopped beating is unplugged without a request, unconfirmed, "
        "and its channel is let go, so the next plug reaches a new helper");
    FakeHelper fake(watchingHelper(PLUG_REPLY_WITH_WATCH));
    hm::HelperClient client;
    EXPECT(client.adoptChannelForTest(fake.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(2, GamepadIdentity::DualSense, false, out));
    const int asked = fake.requests();
    const auto start = Clock::now();
    EXPECT(!client.deprovision(2, hm::Teardown::HelperGone));
    EXPECT(elapsedMs(start) < 1000);
    EXPECT_EQ(fake.requests(), asked);
    EXPECT(!client.isReady());

    TEST("teardown: its serial is settled once a new helper's first plug has swept what the old "
         "one left, and only then");
    EXPECT(client.takeSettledSerials().empty());
    FakeHelper next(watchingHelper(PLUG_REPLY_WITH_WATCH));
    EXPECT(client.adoptChannelForTest(next.connect()));
    EXPECT(client.takeSettledSerials().empty());
    EXPECT(client.provision(5, GamepadIdentity::DualSense, false, out));
    EXPECT(client.takeSettledSerials() == std::vector<uint32_t>({2}));
    EXPECT(client.takeSettledSerials().empty());
    client.shutdown();
}

static void test_a_pad_of_a_replaced_helper_never_touches_the_new_one() {
    TEST("teardown: a pad plugged on a helper that has since been replaced is let go without "
         "asking the new helper anything, whatever the reason, and the new helper's channel "
         "stays");
    FakeHelper first(watchingHelper(PLUG_REPLY_WITH_WATCH));
    hm::HelperClient client;
    EXPECT(client.adoptChannelForTest(first.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(2, GamepadIdentity::DualSense, false, out));
    EXPECT(client.provision(4, GamepadIdentity::DualSense, false, out));
    EXPECT(client.provision(6, GamepadIdentity::DualSense, false, out));
    const uint64_t firstChannel = out.channel;
    EXPECT(firstChannel != 0);
    FakeHelper second(watchingHelper(PLUG_REPLY_WITH_WATCH));
    EXPECT(client.adoptChannelForTest(second.connect()));
    const uint64_t secondChannel = client.liveChannel();
    EXPECT(secondChannel != firstChannel);
    const int asked = second.requests();
    EXPECT(!client.deprovision(2, hm::Teardown::HelperGone));
    EXPECT(!client.deprovision(4, hm::Teardown::DeviceLost));
    EXPECT(!client.deprovision(6, hm::Teardown::Requested));
    EXPECT(client.isReady());
    EXPECT_EQ(client.liveChannel(), secondChannel);
    EXPECT_EQ(second.requests(), asked);

    TEST("teardown: the new helper's first plug, which swept what the old one left, settles "
         "those serials");
    EXPECT(client.provision(8, GamepadIdentity::DualSense, false, out));
    EXPECT_EQ(out.channel, secondChannel);
    EXPECT(client.takeSettledSerials() == std::vector<uint32_t>({2, 4, 6}));
    EXPECT(second.idleWithin(2000));
    EXPECT(!second.closedByClient());

    TEST("teardown: a pad of the current helper that stopped beating lets that channel go, and "
         "no channel is live after it");
    EXPECT(!client.deprovision(8, hm::Teardown::HelperGone));
    EXPECT(!client.isReady());
    EXPECT_EQ(client.liveChannel(), uint64_t{0});
    client.shutdown();
}

static void test_a_requested_unplug_that_fails_is_settled_by_the_next_helper_too() {
    TEST(
        "teardown: an unplug the client asked for that its helper never answers drops the channel, "
        "and the next helper's first plug settles its serial");
    FakeHelper fake(silentAfterHello());
    hm::HelperClient client;
    client.setPipeWaitForTest(SHORT_WAIT_MS);
    EXPECT(client.adoptChannelForTest(fake.connect()));
    EXPECT(!client.deprovision(6, hm::Teardown::Requested));
    EXPECT(!client.isReady());
    FakeHelper next(watchingHelper(PLUG_REPLY_WITH_WATCH));
    EXPECT(client.adoptChannelForTest(next.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(7, GamepadIdentity::DualSense, false, out));
    EXPECT(client.takeSettledSerials() == std::vector<uint32_t>({6}));
    client.shutdown();
}

static void test_a_helper_that_predates_the_watch_stays_on_the_base_protocol() {
    TEST("plug reply: a helper that names no protocol at hello is on the base protocol, so no "
         "watch block is taken from its plug, and one it sent anyway is closed");
    const NamedSection stray(L"stray-watch");
    const std::string reply = PLUG_REPLY.substr(0, PLUG_REPLY.size() - 1) + R"(,"watch":)" +
                              std::to_string(stray.handleValue()) + "}";
    FakeHelper fake(answersHelloThen([reply](FakeHelper& helper) { helper.write(reply + "\n"); }));
    hm::HelperClient client;
    EXPECT(client.adoptChannelForTest(fake.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(3, GamepadIdentity::Xbox, false, out));
    EXPECT_EQ(out.watchSection, static_cast<uint64_t>(0));
    EXPECT(!out.watchReported);
    EXPECT(!stray.alive());
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
    EXPECT(!client.deprovision(3, hm::Teardown::Requested));
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
    EXPECT(!other.deprovision(3, hm::Teardown::Requested));
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

    TEST("limits: a shutdown that is never answered gives up at its own bound");
    {
        FakeHelper fake(silentAfterHello());
        hm::HelperClient client;
        client.setPipeWaitForTest(LONG_WAIT_MS);
        client.setShutdownWaitForTest(SHORT_WAIT_MS);
        EXPECT(client.adoptChannelForTest(fake.connect()));
        const auto start = Clock::now();
        client.shutdown();
        EXPECT(elapsedMs(start) < TIMEOUT_SLACK.count());
        EXPECT(!client.isReady());
    }
}

// A helper that answers a shutdown only once the removals it answered earlier
// are done, `delayMs` after the request.
FakeHelper::Handler finishesRemovalsFirst(DWORD delayMs, std::atomic<bool>* answered) {
    return [delayMs, answered](FakeHelper& helper, const std::string& request) {
        if (isHello(request)) {
            helper.write(HELLO_REPLY_WATCHING + "\n");
        } else if (request.find("\"shutdown\"") != std::string::npos) {
            helper.pause(delayMs);
            *answered = helper.write(OK_REPLY + "\n");
        } else if (request.find("\"unplug\"") != std::string::npos) {
            helper.write(OK_REPLY + "\n");
        } else {
            helper.write(PLUG_REPLY_WITH_WATCH + "\n");
        }
    };
}

static void test_a_shutdown_waits_for_the_helpers_removals() {
    TEST("shutdown: satellite.exe waits for a helper still removing the pads it unplugged, past "
         "every other request's bound, so it is never ended mid-removal");
    std::atomic<bool> answered{false};
    FakeHelper fake(finishesRemovalsFirst(1000, &answered));
    hm::HelperClient client;
    client.setPipeWaitForTest(SHORT_WAIT_MS);
    client.setShutdownWaitForTest(LONG_WAIT_MS);
    EXPECT(client.adoptChannelForTest(fake.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(2, GamepadIdentity::DualSense, false, out));
    EXPECT(client.deprovision(2, hm::Teardown::Requested));
    const auto start = Clock::now();
    client.shutdown();
    EXPECT(elapsedMs(start) >= 900);
    EXPECT(answered.load());
}

static void test_a_shutdown_is_sent_behind_an_owed_answer() {
    TEST("shutdown: a shutdown with a lost pad's unplug answer still owed is sent once that "
         "answer comes, within the shutdown's own bound, so the helper is told to finish its "
         "removals rather than ended");
    std::atomic<bool> told{false};
    FakeHelper fake([&told](FakeHelper& helper, const std::string& request) {
        if (isHello(request)) {
            helper.write(HELLO_REPLY_WATCHING + "\n");
        } else if (request.find("\"shutdown\"") != std::string::npos) {
            told = true;
            helper.write(OK_REPLY + "\n");
        } else if (request.find("\"unplug\"") != std::string::npos) {
            helper.pause(800);
            helper.write(OK_REPLY + "\n");
        } else {
            helper.write(PLUG_REPLY_WITH_WATCH + "\n");
        }
    });
    hm::HelperClient client;
    client.setLostUnplugWaitForTest(20);
    client.setPipeWaitForTest(LONG_WAIT_MS);
    client.setOwedWaitForTest(SHORT_WAIT_MS);
    client.setShutdownWaitForTest(LONG_WAIT_MS);
    EXPECT(client.adoptChannelForTest(fake.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(2, GamepadIdentity::DualSense, false, out));
    EXPECT(!client.deprovision(2, hm::Teardown::DeviceLost));
    const auto start = Clock::now();
    client.shutdown();
    EXPECT(told.load());
    EXPECT(elapsedMs(start) < TIMEOUT_SLACK.count());
    EXPECT(client.takeSettledSerials() == std::vector<uint32_t>({2}));
}

static void test_an_answer_that_lands_during_the_cancel_is_kept() {
    TEST("pipe: an answer whose read completes while its wait is being cancelled is kept, so a "
         "late answer is never lost to the cancel");
    FakeHelper fake([](FakeHelper& helper, const std::string& request) {
        if (isHello(request)) {
            helper.write(HELLO_REPLY_WATCHING + "\n");
        } else if (request.find("\"unplug\"") == std::string::npos) {
            helper.write(PLUG_REPLY_WITH_WATCH + "\n");
        }
    });
    hm::HelperClient client;
    client.setLostUnplugWaitForTest(50);
    client.setPipeWaitForTest(SHORT_WAIT_MS);
    client.setProvisionWaitForTest(SHORT_WAIT_MS);
    EXPECT(client.adoptChannelForTest(fake.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(2, GamepadIdentity::DualSense, false, out));
    hm::setBeforeCancelForTest([&fake] {
        fake.write(OK_REPLY + "\n");
        std::this_thread::sleep_for(milliseconds(200));
    });
    EXPECT(client.deprovision(2, hm::Teardown::DeviceLost));
    EXPECT(client.provision(3, GamepadIdentity::DualSense, false, out));
    EXPECT(client.isReady());
    client.shutdown();
}

static void test_an_owed_answer_waits_its_own_small_bound() {
    TEST("owed: an answer owed to a lost pad's unplug that never comes fails the next request "
         "after the owed bound, not after that request's own, so a plug holds the topology lock "
         "no longer than that");
    FakeHelper fake([](FakeHelper& helper, const std::string& request) {
        if (isHello(request)) {
            helper.write(HELLO_REPLY_WATCHING + "\n");
        } else if (request.find("\"unplug\"") == std::string::npos) {
            helper.write(PLUG_REPLY_WITH_WATCH + "\n");
        }
    });
    hm::HelperClient client;
    client.setLostUnplugWaitForTest(20);
    client.setPipeWaitForTest(1500);
    client.setProvisionWaitForTest(LONG_WAIT_MS);
    client.setOwedWaitForTest(SHORT_WAIT_MS);
    EXPECT(client.adoptChannelForTest(fake.connect()));
    hm::ProvisionResult out;
    EXPECT(client.provision(2, GamepadIdentity::DualSense, false, out));
    EXPECT(!client.deprovision(2, hm::Teardown::DeviceLost));
    const auto start = Clock::now();
    EXPECT(!client.provision(3, GamepadIdentity::DualSense, false, out));
    EXPECT(elapsedMs(start) < TIMEOUT_SLACK.count());
    TEST("owed: while the owed answer is younger than an unplug's own bound, the request fails "
         "unsent and the channel stays");
    EXPECT(client.isReady());
    EXPECT(!fake.closedByClient());

    TEST("owed: once it is older than that, the helper no longer answers, and the next request "
         "lets the channel go");
    std::this_thread::sleep_for(milliseconds(1600));
    EXPECT(!client.provision(4, GamepadIdentity::DualSense, false, out));
    EXPECT(!client.isReady());
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
    struct LostUnplug {
        bool plugged = false;
        bool confirmed = true;
        long long tookMs = 0;
        bool keptChannel = false;
        bool closedByClient = true;
        bool meanwhileFailsFast = false;
        bool nextPlug = false;
        std::vector<uint32_t> settled;
    } lost;
    Outcome slowExit;
    std::atomic<bool> exitAnswered{false};

    std::thread stalledCall([&stalled] {
        FakeHelper fake(silentAfterHello());
        hm::HelperClient client;
        client.adoptChannelForTest(fake.connect());
        const auto start = Clock::now();
        stalled.ok = client.deprovision(3, hm::Teardown::Requested);
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
    std::thread lostCall([&lost] {
        FakeHelper fake(slowUnplug(REAL_SLOW_MS));
        hm::HelperClient client;
        client.adoptChannelForTest(fake.connect());
        hm::ProvisionResult healthy;
        hm::ProvisionResult gone;
        hm::ProvisionResult later;
        lost.plugged = client.provision(1, GamepadIdentity::DualSense, false, healthy) &&
                       client.provision(2, GamepadIdentity::DualSense, false, gone);
        const auto start = Clock::now();
        lost.confirmed = client.deprovision(2, hm::Teardown::DeviceLost);
        lost.tookMs = elapsedMs(start);
        lost.keptChannel = client.isReady();
        const auto meanwhile = Clock::now();
        const bool meanwhilePlugged = client.provision(3, GamepadIdentity::DualSense, false, later);
        lost.meanwhileFailsFast =
            !meanwhilePlugged && elapsedMs(meanwhile) < 4000 && client.isReady();
        fake.idleWithin(15000);
        lost.nextPlug = client.provision(5, GamepadIdentity::DualSense, false, later);
        lost.settled = client.takeSettledSerials();
        lost.closedByClient = !fake.idleWithin(5000) || fake.closedByClient();
        client.shutdown();
    });
    std::thread slowShutdownCall([&slowExit, &exitAnswered] {
        FakeHelper fake(finishesRemovalsFirst(REAL_SLOW_MS, &exitAnswered));
        hm::HelperClient client;
        client.adoptChannelForTest(fake.connect());
        hm::ProvisionResult out;
        slowExit.ok = client.provision(2, GamepadIdentity::DualSense, false, out) &&
                      client.deprovision(2, hm::Teardown::Requested);
        const auto start = Clock::now();
        client.shutdown();
        slowExit.tookMs = elapsedMs(start);
    });
    stalledCall.join();
    slowPlugCall.join();
    slowLaunchCall.join();
    lostCall.join();
    slowShutdownCall.join();

    TEST("limits (real time): a shutdown its helper answers only after more than ten seconds, "
         "once removals that outlast the 5 s exit wait are done, is waited for, so the helper is "
         "never ended mid-removal");
    EXPECT(slowExit.ok);
    EXPECT(exitAnswered.load());
    EXPECT(slowExit.tookMs >= 10000);
    EXPECT(slowExit.tookMs < 60000);

    TEST("limits (real time): a lost pad's unplug that its helper answers only after more than "
         "ten seconds returns within its short wait, unconfirmed for now");
    EXPECT(lost.plugged);
    EXPECT(!lost.confirmed);
    EXPECT(lost.tookMs < 1000);
    TEST("limits (real time): that unplug keeps the channel, so the helper's other pads keep their "
         "session and their heartbeat");
    EXPECT(lost.keptChannel);
    EXPECT(!lost.closedByClient);
    TEST("limits (real time): a plug meanwhile fails within the owed answer's 2 s and keeps the "
         "channel");
    EXPECT(lost.meanwhileFailsFast);
    TEST("limits (real time): the late answer is read before the next request's, which succeeds, "
         "and it settles the lost pad's serial");
    EXPECT(lost.nextPlug);
    EXPECT(lost.settled == std::vector<uint32_t>({2}));

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
    test_a_reply_names_where_the_frames_go();
    test_a_reply_hands_over_the_version_section();
    test_the_hello_offers_the_device_watch();
    test_a_plug_on_the_watch_protocol_hands_over_its_block();
    test_a_helper_that_predates_the_watch_stays_on_the_base_protocol();
    test_a_lost_pads_unplug_answered_at_once_is_confirmed();
    test_a_second_lost_unplug_does_not_wait_behind_the_first();
    test_a_pad_whose_helper_stopped_is_let_go_without_asking_it();
    test_a_requested_unplug_that_fails_is_settled_by_the_next_helper_too();
    test_a_pad_of_a_replaced_helper_never_touches_the_new_one();
    test_a_reply_costs_no_more_reads_than_it_arrives_in_pieces();
    test_a_carriage_return_before_the_line_feed_is_part_of_the_framing();
    test_a_reply_with_no_end_is_refused_at_the_size_limit();
    test_a_silent_helper_fails_the_call_at_the_deadline();
    test_a_half_line_is_not_a_reply();
    test_the_deadline_covers_the_whole_reply();
    test_a_slow_first_provision_succeeds_within_its_limit();
    test_a_plug_that_stalls_still_fails_at_its_limit();
    test_hello_and_shutdown_keep_the_short_bound_beside_a_long_plug();
    test_a_shutdown_waits_for_the_helpers_removals();
    test_a_shutdown_is_sent_behind_an_owed_answer();
    test_an_answer_that_lands_during_the_cancel_is_kept();
    test_an_owed_answer_waits_its_own_small_bound();
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
