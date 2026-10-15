// SPDX-License-Identifier: LGPL-3.0-or-later
#include "net/datagram_receiver.h"
#include "net/receive_wake.h"

#include <mmsystem.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"

namespace {

constexpr int DATAGRAM_BYTES = 41;
constexpr std::size_t READ_BYTES = 1500;
constexpr uint8_t FILL = 0xA5;
constexpr uint8_t SENTINEL = 0xCD;
constexpr unsigned IDLE_TIMEOUT_MS = 20;
constexpr unsigned STREAM_TIMEOUT_MS = 1;
constexpr int GAP_CENTER_US = 1000;
constexpr int GAP_SPREAD_US = 300;
constexpr auto STREAM_DURATION = std::chrono::seconds(2);
constexpr auto LEGACY_MAX_DURATION = std::chrono::seconds(5);
constexpr uint64_t LEGACY_ENOUGH_LOSSES = 5;
constexpr int IDLE_TIMEOUTS_TO_DRAIN = 3;
constexpr int EXIT_SKIP = 77;

SOCKET bindLoopback(sockaddr_in& addr) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    return s;
}

struct Loopback {
    sockaddr_in rxAddr{};
    sockaddr_in txAddr{};
    SOCKET rx = bindLoopback(rxAddr);
    SOCKET tx = bindLoopback(txAddr);

    Loopback() {
        const int rcvBuf = 1 << 20;
        setsockopt(rx, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf),
                   sizeof(rcvBuf));
    }
    ~Loopback() {
        closesocket(tx);
        closesocket(rx);
    }
    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;

    bool send(const std::vector<uint8_t>& bytes) const {
        const int len = static_cast<int>(bytes.size());
        return sendto(tx, reinterpret_cast<const char*>(bytes.data()), len, 0,
                      reinterpret_cast<const sockaddr*>(&rxAddr), sizeof(rxAddr)) == len;
    }
};

std::vector<uint8_t> datagram(uint32_t seq) {
    std::vector<uint8_t> d(DATAGRAM_BYTES, FILL);
    std::memcpy(d.data(), &seq, sizeof(seq));
    return d;
}

bool holdsDatagram(const uint8_t* b, uint32_t& seq) {
    std::memcpy(&seq, b, sizeof(seq));
    for (int i = static_cast<int>(sizeof(seq)); i < DATAGRAM_BYTES; i++) {
        if (b[i] != FILL) return false;
    }
    return true;
}

bool untouched(const std::vector<uint8_t>& buf) {
    for (uint8_t b : buf) {
        if (b != SENTINEL) return false;
    }
    return true;
}

bool sameAddress(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_family == b.sin_family && a.sin_port == b.sin_port &&
           a.sin_addr.s_addr == b.sin_addr.s_addr;
}

bool ioPending() {
    BOOL pending = FALSE;
    GetThreadIOPendingFlag(GetCurrentThread(), &pending);
    return pending != FALSE;
}

class JitteredSender {
  public:
    JitteredSender(const Loopback& link, std::chrono::steady_clock::duration duration)
        : thread_([this, &link, duration] { run(link, duration); }) {}
    ~JitteredSender() { stop(); }
    JitteredSender(const JitteredSender&) = delete;
    JitteredSender& operator=(const JitteredSender&) = delete;

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }
    bool done() const { return done_.load(); }
    uint32_t sent() const { return sent_.load(); }

  private:
    std::atomic<bool> stop_{false};
    std::atomic<bool> done_{false};
    std::atomic<uint32_t> sent_{0};
    std::thread thread_;

    void run(const Loopback& link, std::chrono::steady_clock::duration duration) {
        std::mt19937 rng(20261014);
        std::uniform_int_distribution<int> gapUs(GAP_CENTER_US - GAP_SPREAD_US,
                                                 GAP_CENTER_US + GAP_SPREAD_US);
        const auto end = std::chrono::steady_clock::now() + duration;
        while (!stop_.load() && std::chrono::steady_clock::now() < end) {
            if (link.send(datagram(sent_.load()))) sent_.fetch_add(1);
            const auto until =
                std::chrono::steady_clock::now() + std::chrono::microseconds(gapUs(rng));
            while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
        }
        done_.store(true);
    }
};

void test_idle_link_reports_timeouts() {
    TEST("an idle link reports each timeout and keeps the receive pending");
    Loopback link;
    DatagramReceiver rx(link.rx, READ_BYTES, IDLE_TIMEOUT_MS);
    for (int i = 0; i < 3; i++) {
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_EQ(rx.receive(), -1);
        EXPECT(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
        EXPECT(ioPending());
    }
}

void test_first_datagram_after_a_timeout_is_delivered() {
    TEST("the first datagram after a timeout is delivered with its sender");
    Loopback link;
    DatagramReceiver rx(link.rx, READ_BYTES, IDLE_TIMEOUT_MS);
    EXPECT_EQ(rx.receive(), -1);
    EXPECT(link.send(datagram(7)));
    EXPECT_EQ(rx.receive(), DATAGRAM_BYTES);
    uint32_t seq = 0;
    EXPECT(holdsDatagram(rx.data(), seq));
    EXPECT_EQ(seq, 7u);
    EXPECT(sameAddress(rx.sender(), link.txAddr));
}

void test_queued_datagrams_are_delivered_in_turn() {
    TEST("datagrams queued before the first receive are each delivered once");
    Loopback link;
    for (uint32_t s = 0; s < 3; s++) EXPECT(link.send(datagram(s)));
    DatagramReceiver rx(link.rx, READ_BYTES, IDLE_TIMEOUT_MS);
    for (uint32_t s = 0; s < 3; s++) {
        EXPECT_EQ(rx.receive(), DATAGRAM_BYTES);
        uint32_t seq = 0;
        EXPECT(holdsDatagram(rx.data(), seq));
        EXPECT_EQ(seq, s);
        EXPECT(sameAddress(rx.sender(), link.txAddr));
    }
    EXPECT_EQ(rx.receive(), -1);
}

void test_an_oversized_datagram_does_not_wedge_the_receiver() {
    TEST("a datagram larger than the buffer reads as -1 and the next one still arrives");
    Loopback link;
    const std::vector<uint8_t> oversized(100, FILL);
    DatagramReceiver rx(link.rx, 64, IDLE_TIMEOUT_MS);
    EXPECT(link.send(oversized));
    EXPECT_EQ(rx.receive(), -1);
    EXPECT_EQ(rx.receive(), -1);
    EXPECT(link.send(oversized));
    EXPECT_EQ(rx.receive(), -1);
    EXPECT(link.send(datagram(5)));
    EXPECT_EQ(rx.receive(), DATAGRAM_BYTES);
    uint32_t seq = 0;
    EXPECT(holdsDatagram(rx.data(), seq));
    EXPECT_EQ(seq, 5u);
}

void test_destroying_the_receiver_leaves_nothing_outstanding() {
    TEST("destroying the receiver drains its pending receive; a fresh reader gets the next "
         "datagram");
    Loopback link;
    {
        DatagramReceiver rx(link.rx, READ_BYTES, IDLE_TIMEOUT_MS);
        EXPECT_EQ(rx.receive(), -1);
        EXPECT(ioPending());
    }
    EXPECT(!ioPending());
    EXPECT(link.send(datagram(11)));
    DatagramReceiver fresh(link.rx, READ_BYTES, 1000);
    EXPECT_EQ(fresh.receive(), DATAGRAM_BYTES);
    uint32_t seq = 0;
    EXPECT(holdsDatagram(fresh.data(), seq));
    EXPECT_EQ(seq, 11u);
}

void test_a_receiver_outliving_its_socket_still_drains() {
    TEST("a receiver whose socket closed first still drains its receive on destruction");
    sockaddr_in addr{};
    const SOCKET sock = bindLoopback(addr);
    {
        DatagramReceiver rx(sock, READ_BYTES, IDLE_TIMEOUT_MS);
        EXPECT_EQ(rx.receive(), -1);
        EXPECT(ioPending());
        closesocket(sock);
    }
    EXPECT(!ioPending());
}

void test_a_jittered_stream_loses_nothing() {
    TEST("a stream jittered around the timeout loses nothing and names its sender");
    Loopback link;
    DatagramReceiver rx(link.rx, READ_BYTES, STREAM_TIMEOUT_MS);
    JitteredSender sender(link, STREAM_DURATION);
    std::set<uint32_t> delivered;
    uint64_t timeouts = 0;
    uint64_t duplicates = 0;
    uint64_t malformed = 0;
    uint64_t wrongSender = 0;
    int idle = 0;
    while (idle < IDLE_TIMEOUTS_TO_DRAIN) {
        const int n = rx.receive();
        if (n < 0) {
            timeouts++;
            if (sender.done()) idle++;
            continue;
        }
        idle = 0;
        uint32_t seq = 0;
        if (n != DATAGRAM_BYTES || !holdsDatagram(rx.data(), seq)) {
            malformed++;
            continue;
        }
        if (!delivered.insert(seq).second) duplicates++;
        if (!sameAddress(rx.sender(), link.txAddr)) wrongSender++;
    }
    sender.stop();
    EXPECT(sender.sent() > 0);
    EXPECT_EQ(delivered.size(), static_cast<std::size_t>(sender.sent()));
    EXPECT(delivered.empty() || *delivered.rbegin() < sender.sent());
    EXPECT_EQ(duplicates, 0u);
    EXPECT_EQ(malformed, 0u);
    EXPECT_EQ(wrongSender, 0u);
    std::cout << "  stream: " << sender.sent() << " sent, " << delivered.size() << " delivered, "
              << timeouts << " timeouts\n";
}

bool joinWithin(std::thread& t, const std::atomic<bool>& done, SOCKET& sock,
                std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done.load() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool finished = done.load();
    if (!finished) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    t.join();
    return finished;
}

struct BlockedReceive {
    std::atomic<bool> done{false};
    int n = 0;
    int err = 0;
    bool pendingAfter = false;
    std::thread thread;
};

void receiveOnce(SOCKET sock, ReceiveWake& wake, BlockedReceive& r,
                 std::chrono::milliseconds delay) {
    r.thread = std::thread([sock, &wake, &r, delay] {
        std::this_thread::sleep_for(delay);
        char buf[64];
        r.n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
        r.err = r.n < 0 ? WSAGetLastError() : 0;
        r.pendingAfter = ioPending();
        wake.disarm();
        r.done.store(true);
    });
}

void test_waking_with_no_receiver_listening_returns_at_once() {
    TEST("waking when no receiver is listening returns at once");
    ReceiveWake wake;
    const auto t0 = std::chrono::steady_clock::now();
    wake.wake();
    EXPECT(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100));
}

void test_the_wake_datagram_ends_a_receive_with_no_timeout() {
    TEST("the wake datagram ends a receive that has no timeout and leaves nothing pending");
    sockaddr_in addr{};
    SOCKET sock = bindLoopback(addr);
    ReceiveWake wake;
    wake.arm(sock, ntohs(addr.sin_port));
    BlockedReceive r;
    receiveOnce(sock, wake, r, std::chrono::milliseconds(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t0 = std::chrono::steady_clock::now();
    wake.wake();
    const auto took = std::chrono::steady_clock::now() - t0;
    EXPECT(joinWithin(r.thread, r.done, sock, std::chrono::seconds(5)));
    EXPECT_EQ(r.n, 0);
    EXPECT(!r.pendingAfter);
    EXPECT(took < std::chrono::milliseconds(500));
    closesocket(sock);
}

void test_a_wake_that_lands_before_the_receive_still_ends_it() {
    TEST("a wake that lands before the receive starts still ends it");
    sockaddr_in addr{};
    SOCKET sock = bindLoopback(addr);
    ReceiveWake wake;
    wake.arm(sock, ntohs(addr.sin_port));
    BlockedReceive r;
    receiveOnce(sock, wake, r, std::chrono::milliseconds(50));
    wake.wake();
    EXPECT(joinWithin(r.thread, r.done, sock, std::chrono::seconds(5)));
    EXPECT_EQ(r.n, 0);
    EXPECT(!r.pendingAfter);
    closesocket(sock);
}

void test_cancelling_ends_a_receive_the_wake_datagram_misses() {
    TEST("when the wake datagram cannot arrive, cancelling ends the receive and the socket still "
         "sends");
    sockaddr_in addr{};
    sockaddr_in decoyAddr{};
    SOCKET sock = bindLoopback(addr);
    const SOCKET decoy = bindLoopback(decoyAddr);
    ReceiveWake wake;
    wake.arm(sock, ntohs(decoyAddr.sin_port));
    BlockedReceive r;
    receiveOnce(sock, wake, r, std::chrono::milliseconds(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t0 = std::chrono::steady_clock::now();
    wake.wake();
    const auto took = std::chrono::steady_clock::now() - t0;
    EXPECT(joinWithin(r.thread, r.done, sock, std::chrono::seconds(5)));
    EXPECT_EQ(r.n, -1);
    EXPECT(r.err == WSAEINTR || r.err == WSA_OPERATION_ABORTED);
    EXPECT(!r.pendingAfter);
    EXPECT(took < std::chrono::seconds(1));
    EXPECT_EQ(
        sendto(sock, "x", 1, 0, reinterpret_cast<const sockaddr*>(&decoyAddr), sizeof(decoyAddr)),
        1);
    closesocket(decoy);
    closesocket(sock);
}

void test_a_receive_with_no_timeout_loses_nothing_and_stops_on_wake() {
    TEST("a receive with no timeout loses nothing under the jittered stream and stops on wake");
    Loopback link;
    ReceiveWake wake;
    wake.arm(link.rx, ntohs(link.rxAddr.sin_port));
    std::atomic<bool> running{true};
    std::atomic<bool> stopped{false};
    std::atomic<uint64_t> received{0};
    std::set<uint32_t> delivered;
    uint64_t duplicates = 0;
    uint64_t malformed = 0;
    uint64_t wrongSender = 0;
    uint64_t empty = 0;
    std::thread reader([&] {
        while (running.load()) {
            sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            uint8_t buf[READ_BYTES];
            const int n = recvfrom(link.rx, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&sender), &slen);
            if (n == 0) {
                empty++;
                continue;
            }
            uint32_t seq = 0;
            if (n != DATAGRAM_BYTES || !holdsDatagram(buf, seq)) {
                malformed++;
                continue;
            }
            if (!delivered.insert(seq).second) duplicates++;
            if (!sameAddress(sender, link.txAddr)) wrongSender++;
            received.fetch_add(1);
        }
        wake.disarm();
        stopped.store(true);
    });
    uint32_t sent = 0;
    {
        JitteredSender sender(link, STREAM_DURATION);
        while (!sender.done()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        sender.stop();
        sent = sender.sent();
    }
    const auto drainEnd = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.load() < sent && std::chrono::steady_clock::now() < drainEnd)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    running.store(false);
    const auto t0 = std::chrono::steady_clock::now();
    wake.wake();
    const auto took = std::chrono::steady_clock::now() - t0;
    EXPECT(joinWithin(reader, stopped, link.rx, std::chrono::seconds(5)));
    EXPECT(sent > 0);
    EXPECT_EQ(delivered.size(), static_cast<std::size_t>(sent));
    EXPECT_EQ(duplicates, 0u);
    EXPECT_EQ(malformed, 0u);
    EXPECT_EQ(wrongSender, 0u);
    EXPECT(empty <= 1);
    EXPECT(took < std::chrono::milliseconds(500));
    std::cout << "  no-timeout stream: " << sent << " sent, " << delivered.size() << " delivered\n";
}

struct LegacyRead {
    std::vector<uint8_t> buf = std::vector<uint8_t>(READ_BYTES, SENTINEL);
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    bool heldAtReturn = false;
    bool pendingAtReturn = false;
};

int runLegacy() {
    TEST("legacy SO_RCVTIMEO + recvfrom under the same jittered stream");
    Loopback link;
    netSetRecvTimeoutMs(link.rx, STREAM_TIMEOUT_MS);
    JitteredSender sender(link, LEGACY_MAX_DURATION);
    std::set<uint32_t> delivered;
    std::vector<std::unique_ptr<LegacyRead>> timedOut;
    uint64_t heldAtReturn = 0;
    uint64_t pendingAtReturn = 0;
    int idle = 0;
    while (idle < IDLE_TIMEOUTS_TO_DRAIN) {
        auto read = std::make_unique<LegacyRead>();
        const int n = recvfrom(link.rx, reinterpret_cast<char*>(read->buf.data()),
                               static_cast<int>(read->buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&read->from), &read->fromLen);
        const int err = n < 0 ? WSAGetLastError() : 0;
        const bool pending = ioPending();
        if (n >= 0) {
            idle = 0;
            uint32_t seq = 0;
            if (n == DATAGRAM_BYTES && holdsDatagram(read->buf.data(), seq)) delivered.insert(seq);
            continue;
        }
        if (sender.done()) idle++;
        if (err != WSAETIMEDOUT) continue;
        read->pendingAtReturn = pending;
        read->heldAtReturn = !untouched(read->buf);
        if (read->heldAtReturn) heldAtReturn++;
        if (read->pendingAtReturn) pendingAtReturn++;
        timedOut.push_back(std::move(read));
        if (heldAtReturn >= LEGACY_ENOUGH_LOSSES) sender.stop();
    }
    sender.stop();

    std::set<uint32_t> held;
    uint64_t writtenAfterReturn = 0;
    uint64_t heldButDelivered = 0;
    for (const auto& read : timedOut) {
        if (untouched(read->buf)) continue;
        if (!read->heldAtReturn) writtenAfterReturn++;
        uint32_t seq = 0;
        if (!holdsDatagram(read->buf.data(), seq)) continue;
        held.insert(seq);
        if (delivered.count(seq) != 0) heldButDelivered++;
    }
    const uint64_t lost = sender.sent() - delivered.size();
    std::cout << "  legacy: " << sender.sent() << " sent, " << delivered.size() << " delivered, "
              << lost << " lost; " << timedOut.size() << " reads timed out, " << heldAtReturn
              << " of them holding a datagram at return, " << writtenAfterReturn
              << " written after return, " << pendingAtReturn
              << " with a receive still pending at return; " << held.size()
              << " of the lost datagrams sat in timed-out buffers\n";
    EXPECT_EQ(heldButDelivered, 0u);
    EXPECT(lost >= held.size());
    if (g_fail > 0) return 1;
    if (held.empty()) {
        std::cout << "  legacy: the timeout race did not fire on this machine; skipping\n";
        return EXIT_SKIP;
    }
    return 0;
}

int runSuite() {
    test_idle_link_reports_timeouts();
    test_first_datagram_after_a_timeout_is_delivered();
    test_queued_datagrams_are_delivered_in_turn();
    test_an_oversized_datagram_does_not_wedge_the_receiver();
    test_destroying_the_receiver_leaves_nothing_outstanding();
    test_a_receiver_outliving_its_socket_still_drains();
    test_a_jittered_stream_loses_nothing();
    test_waking_with_no_receiver_listening_returns_at_once();
    test_the_wake_datagram_ends_a_receive_with_no_timeout();
    test_a_wake_that_lands_before_the_receive_still_ends_it();
    test_cancelling_ends_a_receive_the_wake_datagram_misses();
    test_a_receive_with_no_timeout_loses_nothing_and_stops_on_wake();
    return g_fail > 0 ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    const bool legacy = argc > 1 && std::string(argv[1]) == "--legacy";
    std::cout << "=== test_udp_receive" << (legacy ? " --legacy" : "") << " ===\n";
    netInit();
    timeBeginPeriod(1);
    const int rc = legacy ? runLegacy() : runSuite();
    timeEndPeriod(1);
    netShutdown();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << (rc == EXIT_SKIP ? "  STATUS: SKIPPED\n" : "  STATUS: ALL PASSED\n");
    return rc;
}
