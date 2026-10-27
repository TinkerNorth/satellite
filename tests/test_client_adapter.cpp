// SPDX-License-Identifier: LGPL-3.0-or-later
#include "adapters/client_adapter.h"
#include "core/byte_order.h"
#include "core/hot_table.h"
#include "net/session_crypto.h"

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "test_util.h"

namespace {

constexpr uint32_t TOKEN = 0x5A7E1100u;
constexpr int SENDS_PER_THREAD = 1500;
constexpr unsigned RECEIVE_TIMEOUT_MS = 200;
constexpr int RECEIVE_BUFFER_BYTES = 8 * 1024 * 1024;
constexpr uint32_t LOOPBACK_HOST_ORDER = 0x7F000001u;

struct Packet {
    uint32_t token = 0;
    uint32_t counter = 0;
    uint16_t msgType = 0;
    std::vector<uint8_t> payload;
};

class Loopback {
  public:
    Loopback() {
        receiver_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sender_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (receiver_ == INVALID_SOCKET || sender_ == INVALID_SOCKET) return;
        const int rcvBuf = RECEIVE_BUFFER_BYTES;
        setsockopt(receiver_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf),
                   sizeof(rcvBuf));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(LOOPBACK_HOST_ORDER);
        addr.sin_port = 0;
        if (bind(receiver_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            return;
        }
        socklen_t length = sizeof(addr);
        if (getsockname(receiver_, reinterpret_cast<sockaddr*>(&addr), &length) == SOCKET_ERROR) {
            return;
        }
        port_ = ntohs(addr.sin_port);
        netSetRecvTimeoutMs(receiver_, RECEIVE_TIMEOUT_MS);
        ready_ = true;
    }

    ~Loopback() {
        if (receiver_ != INVALID_SOCKET) closesocket(receiver_);
        if (sender_ != INVALID_SOCKET) closesocket(sender_);
    }

    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;

    bool ready() const { return ready_; }
    SOCKET sender() const { return sender_; }
    uint64_t clientAddr() const { return packClientAddr(htonl(LOOPBACK_HOST_ORDER), port_); }

    bool receive(const uint8_t key[CRYPTO_KEY_SIZE], Packet& out) {
        uint8_t buf[UDP_DATAGRAM_MAX_BYTES];
        const int n =
            static_cast<int>(recv(receiver_, reinterpret_cast<char*>(buf), sizeof(buf), 0));
        if (n < HEADER_SIZE + INNER_HEADER_SIZE + AUTH_TAG_SIZE) return false;
        out.token = satellite::readBE32(buf);
        out.counter = satellite::readBE32(buf + 4);
        uint8_t plain[UDP_DATAGRAM_MAX_BYTES];
        unsigned long long plainLen = 0;
        if (!decryptPacket(key, CRYPTO_DIR_SERVER_TO_CLIENT, out.counter, out.token,
                           buf + HEADER_SIZE, static_cast<size_t>(n - HEADER_SIZE), plain,
                           &plainLen)) {
            return false;
        }
        out.msgType = satellite::readBE16(plain);
        out.payload.assign(plain + INNER_HEADER_SIZE, plain + plainLen);
        return true;
    }

  private:
    SOCKET receiver_ = INVALID_SOCKET;
    SOCKET sender_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    bool ready_ = false;
};

struct Session {
    HotSlot slot;
    Connection conn;
    SessionEndpoint endpoint;

    explicit Session(uint64_t clientAddr) {
        for (int i = 0; i < CRYPTO_KEY_SIZE; i++) conn.sessionKey[i] = static_cast<uint8_t>(i * 7);
        conn.token = TOKEN;
        conn.hot = &slot;
        slot.clientAddr.store(clientAddr);
        endpoint.token = TOKEN;
        std::memcpy(endpoint.key, conn.sessionKey, CRYPTO_KEY_SIZE);
        endpoint.slot = &slot;
    }
};

void test_aDeferredAckLeavesRightAfterTheSendHoldingTheGate() {
    TEST("client adapter: a heartbeat ack never waits for a send in progress");
    Loopback loop;
    EXPECT(loop.ready());
    if (!loop.ready()) return;
    ClientAdapter client;
    client.setSocket(loop.sender());
    Session session(loop.clientAddr());

    session.slot.txGate.store(1);
    client.sendHeartbeatAck(session.endpoint, true, 3, 0x1234, 0x0005);
    Packet packet;
    EXPECT(!loop.receive(session.conn.sessionKey, packet));
    EXPECT_EQ(session.slot.txCounter.load(), 0u);

    TEST("client adapter: the deferred ack goes out right after the send that held the gate");
    session.slot.txGate.store(0);
    RumbleReport rumble{};
    rumble.strongMagnitude = 0x2222;
    client.sendRumble(session.conn, 1, rumble);
    EXPECT(loop.receive(session.conn.sessionKey, packet));
    EXPECT_EQ(packet.msgType, MSG_RUMBLE);
    EXPECT_EQ(packet.counter, 1u);
    EXPECT(loop.receive(session.conn.sessionKey, packet));
    EXPECT_EQ(packet.msgType, MSG_HEARTBEAT_ACK);
    EXPECT_EQ(packet.counter, 2u);
    EXPECT_EQ(packet.token, TOKEN);
    EXPECT_EQ(packet.payload.size(), size_t{6});
    if (packet.payload.size() == 6) {
        EXPECT_EQ((int)packet.payload[0], 1);
        EXPECT_EQ((int)packet.payload[1], 3);
        EXPECT_EQ(satellite::readBE16(packet.payload.data() + 2), (uint16_t)0x1234);
        EXPECT_EQ(satellite::readBE16(packet.payload.data() + 4), (uint16_t)0x0005);
    }
    EXPECT_EQ(session.slot.txGate.load(), 0u);
    EXPECT_EQ(session.slot.txDeferredAck.load(), 0u);

    TEST("client adapter: an ack with the gate free goes out at once");
    client.sendHeartbeatAck(session.endpoint, false, 0, 7, 0);
    EXPECT(loop.receive(session.conn.sessionKey, packet));
    EXPECT_EQ(packet.msgType, MSG_HEARTBEAT_ACK);
    EXPECT_EQ(packet.counter, 3u);
}

void test_sendersOnSeveralThreadsReachTheClientInCounterOrder() {
    TEST("client adapter: senders on three threads reach the client in counter order");
    Loopback loop;
    EXPECT(loop.ready());
    if (!loop.ready()) return;
    ClientAdapter client;
    client.setSocket(loop.sender());
    Session session(loop.clientAddr());

    std::vector<Packet> received;
    std::atomic<bool> sendersDone{false};
    std::thread receiver([&] {
        Packet packet;
        for (;;) {
            if (loop.receive(session.conn.sessionKey, packet)) {
                received.push_back(packet);
                continue;
            }
            if (sendersDone.load()) return;
        }
    });
    std::thread acks([&] {
        for (int i = 0; i < SENDS_PER_THREAD; i++) {
            client.sendHeartbeatAck(session.endpoint, true, 1, static_cast<uint16_t>(i), 1);
        }
    });
    std::thread rumbles([&] {
        RumbleReport rumble{};
        for (int i = 0; i < SENDS_PER_THREAD; i++) {
            rumble.strongMagnitude = static_cast<uint16_t>(i);
            client.sendRumble(session.conn, 0, rumble);
        }
    });
    std::thread lightbars([&] {
        for (int i = 0; i < SENDS_PER_THREAD; i++) {
            client.sendLightbar(session.conn, 0, static_cast<uint8_t>(i), 0, 0);
        }
    });
    acks.join();
    rumbles.join();
    lightbars.join();
    sendersDone.store(true);
    receiver.join();

    int ackCount = 0;
    int rumbleCount = 0;
    int lightbarCount = 0;
    int outOfOrder = 0;
    uint32_t last = 0;
    for (const Packet& packet : received) {
        if (packet.counter <= last) outOfOrder++;
        last = packet.counter;
        if (packet.msgType == MSG_HEARTBEAT_ACK) ackCount++;
        if (packet.msgType == MSG_RUMBLE) rumbleCount++;
        if (packet.msgType == MSG_LIGHTBAR) lightbarCount++;
    }
    EXPECT_EQ(outOfOrder, 0);
    EXPECT_EQ(rumbleCount, SENDS_PER_THREAD);
    EXPECT_EQ(lightbarCount, SENDS_PER_THREAD);
    EXPECT(ackCount > 0);
    EXPECT(ackCount <= SENDS_PER_THREAD);
    EXPECT_EQ(static_cast<size_t>(session.slot.txCounter.load()), received.size());
    EXPECT_EQ(session.slot.txGate.load(), 0u);
    EXPECT_EQ(session.slot.txDeferredAck.load(), 0u);
    std::cout << "  ordering: " << received.size() << " packets (" << ackCount << " acks, "
              << rumbleCount << " rumbles, " << lightbarCount << " light bars), " << outOfOrder
              << " out of order\n";
}

} // namespace

int main() {
    if (sodium_init() < 0 || !netInit()) {
        std::cerr << "init failed\n";
        return 1;
    }
    test_aDeferredAckLeavesRightAfterTheSendHoldingTheGate();
    test_sendersOnSeveralThreadsReachTheClientInCounterOrder();
    netShutdown();

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
