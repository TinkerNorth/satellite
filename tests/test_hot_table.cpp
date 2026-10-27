// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/hot_table.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

#include "test_util.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto QUIESCENCE_TIMEOUT = std::chrono::seconds(5);
constexpr auto STILL_WAITING_PROBE = std::chrono::milliseconds(50);
constexpr int FLIPS = 200000;

struct Pattern {
    uint32_t token;
    uint32_t keyEpoch;
    uint64_t keyWord;
    uint32_t ctrl;
};

constexpr Pattern PATTERN_A{0x11111111u, 0x11111111u, 0x1111111111111111ull, 0x11111111u};
constexpr Pattern PATTERN_B{0x22222222u, 0x22222222u, 0x2222222222222222ull, 0x22222222u};

void writePattern(HotSlot& slot, const Pattern& p) {
    slot.beginWrite();
    slot.token.store(p.token, std::memory_order_relaxed);
    slot.keyEpoch.store(p.keyEpoch, std::memory_order_relaxed);
    for (auto& word : slot.key) word.store(p.keyWord, std::memory_order_relaxed);
    for (auto& word : slot.ctrl) word.store(p.ctrl, std::memory_order_relaxed);
    slot.endWrite();
}

struct Observed {
    uint32_t token = 0;
    uint32_t keyEpoch = 0;
    uint64_t key[HOT_KEY_WORDS] = {};
    uint32_t ctrl[MAX_CONTROLLERS_PER_CONN] = {};
};

bool readAll(const HotSlot& slot, Observed& out) {
    return slot.read([&] {
        out.token = slot.token.load(std::memory_order_relaxed);
        out.keyEpoch = slot.keyEpoch.load(std::memory_order_relaxed);
        for (size_t i = 0; i < HOT_KEY_WORDS; i++) {
            out.key[i] = slot.key[i].load(std::memory_order_relaxed);
        }
        for (size_t i = 0; i < MAX_CONTROLLERS_PER_CONN; i++) {
            out.ctrl[i] = slot.ctrl[i].load(std::memory_order_relaxed);
        }
    });
}

bool matches(const Observed& seen, const Pattern& p) {
    if (seen.token != p.token || seen.keyEpoch != p.keyEpoch) return false;
    for (uint64_t word : seen.key) {
        if (word != p.keyWord) return false;
    }
    for (uint32_t word : seen.ctrl) {
        if (word != p.ctrl) return false;
    }
    return true;
}

bool waitFor(const std::atomic<bool>& flag, Clock::duration timeout) {
    const auto deadline = Clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire)) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

std::atomic<int> g_barrierCalls{0};

void countingBarrier() { g_barrierCalls.fetch_add(1, std::memory_order_relaxed); }

void test_seqlock_readerFailsDuringWriteAndSucceedsAfter() {
    TEST("seqlock: a read fails while a write is open and succeeds once it closes");
    HotSlot slot;
    Observed seen;
    EXPECT(readAll(slot, seen));
    EXPECT(!slot.writing());

    slot.beginWrite();
    EXPECT(slot.writing());
    EXPECT(!readAll(slot, seen));
    slot.token.store(7, std::memory_order_relaxed);
    EXPECT(!readAll(slot, seen));
    slot.endWrite();

    EXPECT(!slot.writing());
    EXPECT(readAll(slot, seen));
    EXPECT_EQ(seen.token, 7u);
    EXPECT_EQ(slot.gen.load() % 2, 0u);
}

void test_seqlock_readOverlappingAWriteFails() {
    TEST("seqlock: a read whose loads straddle a whole write is refused");
    HotSlot slot;
    uint32_t token = 0;
    const bool ok = slot.read([&] {
        token = slot.token.load(std::memory_order_relaxed);
        slot.beginWrite();
        slot.token.store(9, std::memory_order_relaxed);
        slot.endWrite();
    });
    EXPECT(!ok);
    EXPECT_EQ(token, 0u);
}

void test_seqlock_concurrentReaderNeverSeesAMix() {
    TEST("seqlock: a reader racing a writer sees one whole pattern or nothing");
    auto slot = std::make_unique<HotSlot>();
    writePattern(*slot, PATTERN_A);
    std::atomic<bool> readerStarted{false};
    std::atomic<bool> writerDone{false};
    uint64_t whole = 0;
    uint64_t mixed = 0;
    uint64_t refused = 0;

    std::thread reader([&] {
        readerStarted.store(true, std::memory_order_release);
        Observed seen;
        bool last = false;
        while (!last) {
            last = writerDone.load(std::memory_order_acquire);
            if (!readAll(*slot, seen)) {
                refused++;
                continue;
            }
            if (matches(seen, PATTERN_A) || matches(seen, PATTERN_B)) {
                whole++;
            } else {
                mixed++;
            }
        }
    });
    EXPECT(waitFor(readerStarted, QUIESCENCE_TIMEOUT));
    for (int i = 0; i < FLIPS; i++) writePattern(*slot, (i % 2 == 0) ? PATTERN_B : PATTERN_A);
    writerDone.store(true, std::memory_order_release);
    reader.join();

    EXPECT_EQ(mixed, 0u);
    EXPECT(whole > 0);
    std::cout << "  seqlock race: " << whole << " whole reads, " << refused << " refused, " << mixed
              << " mixed\n";
}

void test_rxEpoch_parity() {
    TEST("epoch: odd exactly while the receive thread is inside a frame");
    RxEpoch rx;
    EXPECT_EQ(rx.value() % 2, 0u);
    rx.enter();
    EXPECT_EQ(rx.value() % 2, 1u);
    rx.exit();
    EXPECT_EQ(rx.value() % 2, 0u);
    for (int i = 0; i < 1000; i++) {
        rx.enter();
        rx.exit();
    }
    EXPECT_EQ(rx.value(), 2002u);
}

void test_rxEpoch_waitQuiescentIdleReturnsAtOnce() {
    TEST("epoch: waiting for quiescence with nobody inside a frame returns at once");
    RxEpoch rx;
    const auto before = Clock::now();
    rx.waitQuiescent();
    EXPECT(Clock::now() - before < STILL_WAITING_PROBE);

    rx.enter();
    rx.exit();
    rx.waitQuiescent();
    EXPECT(Clock::now() - before < STILL_WAITING_PROBE);
}

void test_rxEpoch_injectedBarrierRunsOncePerWait() {
    TEST("epoch: an injected process barrier replaces the fence, once per wait");
    g_barrierCalls.store(0);
    RxEpoch rx(countingBarrier);
    rx.enter();
    rx.exit();
    EXPECT_EQ(g_barrierCalls.load(), 0);
    rx.waitQuiescent();
    rx.waitQuiescent();
    EXPECT_EQ(g_barrierCalls.load(), 2);
}

void expectWaitHeldUntilExit(RxEpoch& rx) {
    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};
    std::atomic<bool> waited{false};
    std::thread frame([&] {
        rx.enter();
        inside.store(true, std::memory_order_release);
        waitFor(release, QUIESCENCE_TIMEOUT);
        rx.exit();
    });
    EXPECT(waitFor(inside, QUIESCENCE_TIMEOUT));
    std::thread cold([&] {
        rx.waitQuiescent();
        waited.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(STILL_WAITING_PROBE);
    EXPECT(!waited.load(std::memory_order_acquire));
    release.store(true, std::memory_order_release);
    const bool returned = waitFor(waited, QUIESCENCE_TIMEOUT);
    frame.join();
    if (!returned) {
        std::cerr << "  FAIL [" << g_currentTest << "] waitQuiescent never returned\n";
        std::exit(1);
    }
    EXPECT(returned);
    cold.join();
}

void test_rxEpoch_waitHeldWhileAFrameIsOpen() {
    TEST("epoch: waiting for quiescence returns only after the open frame exits");
    RxEpoch rx;
    expectWaitHeldUntilExit(rx);
    RxEpoch flushed(countingBarrier);
    expectWaitHeldUntilExit(flushed);
}

void test_hotWrite_holdsTheSlotOddForItsScope() {
    TEST("write guard: the slot is odd for the guard's scope and even after");
    HotSlot slot;
    RxEpoch rx;
    {
        HotWrite write(slot, rx);
        EXPECT(slot.writing());
    }
    EXPECT(!slot.writing());
    EXPECT_EQ(slot.gen.load(), 2u);

    TEST("write guard: closing early ends the write once, and the scope's end adds nothing");
    {
        HotWrite write(slot, rx);
        write.close();
        EXPECT(!slot.writing());
        write.close();
    }
    EXPECT_EQ(slot.gen.load(), 4u);
}

void test_table_findAllocatePublishRetire() {
    TEST("table: an allocated slot is found only once its packed token is published");
    auto table = std::make_unique<HotTable>();
    EXPECT_EQ(table->find(0x1234u), -1);
    EXPECT_EQ(table->find(0), -1);
    const int first = table->findFree();
    EXPECT_EQ(first, 0);

    HotSlot& slot = table->slots[static_cast<size_t>(first)];
    slot.beginWrite();
    slot.token.store(0x1234u, std::memory_order_relaxed);
    EXPECT_EQ(table->findFree(), 1);
    slot.endWrite();
    EXPECT_EQ(table->find(0x1234u), -1);
    EXPECT_EQ(table->findFree(), first);

    table->publish(first, 0x1234u);
    EXPECT_EQ(table->find(0x1234u), first);
    EXPECT_EQ(table->findFree(), 1);
    EXPECT_EQ(table->find(0), -1);

    TEST("table: retiring clears the packed token before the slot's own fields");
    table->retire(first);
    EXPECT_EQ(table->find(0x1234u), -1);
    EXPECT_EQ(slot.token.load(), 0x1234u);
    slot.beginWrite();
    EXPECT_EQ(table->findFree(), 1);
    slot.token.store(0, std::memory_order_relaxed);
    slot.endWrite();
    EXPECT_EQ(table->findFree(), first);
}

void test_table_fullTableHasNoFreeSlot() {
    TEST("table: with every slot published there is no free slot");
    auto table = std::make_unique<HotTable>();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        const int index = table->findFree();
        EXPECT_EQ(index, i);
        table->publish(index, static_cast<uint32_t>(1000 + i));
    }
    EXPECT_EQ(table->findFree(), -1);
    EXPECT_EQ(table->find(1000u + MAX_SESSIONS - 1), MAX_SESSIONS - 1);
    table->retire(5);
    EXPECT_EQ(table->findFree(), 5);
}

void test_wordsAndAddresses() {
    TEST("packing: controller words and client addresses round-trip");
    EXPECT_EQ(hotCtrlWord(false, 7), 0u);
    uint32_t serial = 7;
    const uint32_t word = hotCtrlWord(true, serial);
    EXPECT((word & HOT_CTRL_ACTIVE) != 0);
    EXPECT_EQ(word & HOT_CTRL_SERIAL_MASK, serial);
    uint16_t port = 4242;
    const uint64_t addr = packClientAddr(0x0100007fu, port);
    EXPECT(addr != 0);
    EXPECT_EQ(clientAddrIPv4(addr), 0x0100007fu);
    EXPECT_EQ(clientAddrPort(addr), port);
    const auto at = Clock::now();
    EXPECT(hotTime(hotTicks(at)) == at);
}

void test_slot_keyRoundTrip() {
    TEST("slot: a key stored as words reads back byte for byte");
    HotSlot slot;
    uint8_t key[CRYPTO_KEY_SIZE];
    for (int i = 0; i < CRYPTO_KEY_SIZE; i++) key[i] = static_cast<uint8_t>(i * 7 + 1);
    slot.storeKey(key);
    uint8_t back[CRYPTO_KEY_SIZE] = {};
    slot.loadKey(back);
    EXPECT(std::memcmp(key, back, CRYPTO_KEY_SIZE) == 0);
}

} // namespace

int main() {
    test_seqlock_readerFailsDuringWriteAndSucceedsAfter();
    test_seqlock_readOverlappingAWriteFails();
    test_seqlock_concurrentReaderNeverSeesAMix();
    test_rxEpoch_parity();
    test_rxEpoch_waitQuiescentIdleReturnsAtOnce();
    test_rxEpoch_injectedBarrierRunsOncePerWait();
    test_rxEpoch_waitHeldWhileAFrameIsOpen();
    test_hotWrite_holdsTheSlotOddForItsScope();
    test_table_findAllocatePublishRetire();
    test_table_fullTableHasNoFreeSlot();
    test_wordsAndAddresses();
    test_slot_keyRoundTrip();

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
