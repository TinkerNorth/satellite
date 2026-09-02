// SPDX-License-Identifier: LGPL-3.0-or-later

// platform/windows/hidmaestro_audio_wire — the two controller-audio rings
// between the elevated helper and satellite. The protocol carries no version
// field and the producer on the far side is C# reimplementing this layout, so
// the SIZES and the seqlock rules are the contract: this suite pins every
// offset, the lap rule, the in-progress marker and the torn-slot retry.
// Pure (no <windows.h>), so it runs on every CI platform.
#include "test_util.h"

#include "../src/platform/windows/hidmaestro_audio_wire.h"

#include <cstring>
#include <iostream>
#include <vector>

using namespace satellite::hidmaestro;

namespace {

// A section as the helper hands it over: zero-filled, head at 0.
std::vector<uint8_t> freshSection() { return std::vector<uint8_t>(AUDIO_SECTION_SIZE, 0); }

std::vector<int16_t> ramp(size_t n, int16_t start = 0) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<int16_t>(start + static_cast<int16_t>(i));
    return v;
}

uint32_t u32at(const uint8_t* p, size_t off) {
    uint32_t v;
    std::memcpy(&v, p + off, sizeof(v));
    return v;
}

void storeU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }

uint8_t* slotAt(std::vector<uint8_t>& sec, uint32_t seqno) {
    return sec.data() + AUDIO_RING_HEADER_SIZE +
           static_cast<size_t>((seqno - 1) % AUDIO_RING_SLOTS) * AUDIO_SLOT_SIZE;
}

} // namespace

static void test_layout_offsets() {
    TEST("audio ring offsets are the helper contract");
    EXPECT_EQ(AUDIO_RING_SLOTS, (size_t)32);
    EXPECT_EQ(AUDIO_RING_HEADER_SIZE, (size_t)8);
    EXPECT_EQ(AUDIO_SLOT_SEQNO_OFFSET, (size_t)0);
    EXPECT_EQ(AUDIO_SLOT_SERIAL_OFFSET, (size_t)4);
    EXPECT_EQ(AUDIO_SLOT_SEQ_OFFSET, (size_t)8);
    EXPECT_EQ(AUDIO_SLOT_SAMPLES_OFFSET, (size_t)10);
    EXPECT_EQ(AUDIO_SLOT_DATA_OFFSET, (size_t)12);
    EXPECT_EQ(AUDIO_SLOT_DATA_CAPACITY, (size_t)4096);
    EXPECT_EQ(AUDIO_SLOT_SAMPLE_CAPACITY, (size_t)2048);

    TEST("section SIZE is the layout check, so pin it exactly");
    EXPECT_EQ(AUDIO_SLOT_SIZE, (size_t)4108);
    EXPECT_EQ(AUDIO_SECTION_SIZE, (size_t)131464);
    EXPECT_EQ(AUDIO_SECTION_SIZE, AUDIO_RING_HEADER_SIZE + AUDIO_RING_SLOTS * AUDIO_SLOT_SIZE);

    TEST("a slot holds more than either producer ever batches");
    // 20 ms of 48 kHz stereo is 1920 interleaved samples; mono is 960.
    EXPECT(AUDIO_SLOT_SAMPLE_CAPACITY >= 1920);
}

static void test_write_read_round_trip() {
    TEST("writeAudioSlot -> readNextAudioSlot round-trips serial, seq and PCM");
    std::vector<uint8_t> sec = freshSection();
    EXPECT_EQ(audioRingHead(sec.data()), (uint32_t)0);

    const std::vector<int16_t> pcm = ramp(960, -400);
    EXPECT(writeAudioSlot(sec.data(), 7, 0x1234, pcm.data(), 960));
    EXPECT_EQ(audioRingHead(sec.data()), (uint32_t)1);

    uint32_t last = 0;
    AudioPacket pkt;
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(last, (uint32_t)1);
    EXPECT_EQ(pkt.serial, (uint32_t)7);
    EXPECT_EQ(pkt.seq, (uint16_t)0x1234);
    EXPECT_EQ(pkt.sampleCount, (uint16_t)960);
    EXPECT_EQ(std::memcmp(pkt.data, pcm.data(), 960 * sizeof(int16_t)), 0);

    TEST("a drained ring reports no new data");
    EXPECT(!readNextAudioSlot(sec.data(), last, pkt));
}

static void test_negative_samples_survive() {
    TEST("PCM is copied verbatim: full-scale negative samples are not mangled");
    std::vector<uint8_t> sec = freshSection();
    const std::vector<int16_t> pcm = {-32768, 32767, -1, 0, 256};
    EXPECT(writeAudioSlot(sec.data(), 1, 0, pcm.data(), 5));
    uint32_t last = 0;
    AudioPacket pkt;
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(pkt.sampleCount, (uint16_t)5);
    EXPECT_EQ(pkt.data[0], (int16_t)-32768);
    EXPECT_EQ(pkt.data[1], (int16_t)32767);
    EXPECT_EQ(pkt.data[2], (int16_t)-1);
    EXPECT_EQ(pkt.data[4], (int16_t)256);
}

static void test_in_order_stream() {
    TEST("a burst drains in publish order, one batch per read");
    std::vector<uint8_t> sec = freshSection();
    for (int i = 0; i < 5; ++i) {
        const std::vector<int16_t> pcm(4, static_cast<int16_t>(100 + i));
        EXPECT(writeAudioSlot(sec.data(), 3, static_cast<uint16_t>(i), pcm.data(), 4));
    }
    uint32_t last = 0;
    AudioPacket pkt;
    for (int i = 0; i < 5; ++i) {
        EXPECT(readNextAudioSlot(sec.data(), last, pkt));
        EXPECT_EQ(pkt.seq, (uint16_t)i);
        EXPECT_EQ(pkt.data[0], (int16_t)(100 + i));
    }
    EXPECT(!readNextAudioSlot(sec.data(), last, pkt));
}

static void test_capacity_guard() {
    TEST("a batch larger than the slot is refused rather than truncated");
    std::vector<uint8_t> sec = freshSection();
    const std::vector<int16_t> pcm(AUDIO_SLOT_SAMPLE_CAPACITY + 1, 5);
    EXPECT(!writeAudioSlot(sec.data(), 1, 0, pcm.data(),
                           static_cast<uint16_t>(AUDIO_SLOT_SAMPLE_CAPACITY + 1)));
    // Refused means untouched: no sequence burned, nothing to read.
    EXPECT_EQ(audioRingHead(sec.data()), (uint32_t)0);
    uint32_t last = 0;
    AudioPacket pkt;
    EXPECT(!readNextAudioSlot(sec.data(), last, pkt));

    TEST("an exactly-full batch is accepted");
    EXPECT(writeAudioSlot(sec.data(), 1, 0, pcm.data(),
                          static_cast<uint16_t>(AUDIO_SLOT_SAMPLE_CAPACITY)));
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(pkt.sampleCount, (uint16_t)AUDIO_SLOT_SAMPLE_CAPACITY);

    TEST("a zero-sample batch is a legal publish carrying no PCM");
    EXPECT(writeAudioSlot(sec.data(), 1, 1, nullptr, 0));
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(pkt.sampleCount, (uint16_t)0);
    EXPECT_EQ(pkt.seq, (uint16_t)1);
}

// A reader more than a lap behind has already lost the old batches; for audio
// that is the right outcome (a listener wants the newest samples, not a late
// replay), so the reader jumps to the oldest slot still intact.
static void test_lap_skip() {
    TEST("a reader more than 32 batches behind skips to the oldest readable slot");
    std::vector<uint8_t> sec = freshSection();
    const int total = static_cast<int>(AUDIO_RING_SLOTS) + 5;
    for (int i = 0; i < total; ++i) {
        const std::vector<int16_t> pcm(2, static_cast<int16_t>(i));
        EXPECT(writeAudioSlot(sec.data(), 9, static_cast<uint16_t>(i), pcm.data(), 2));
    }
    uint32_t last = 0; // never read anything
    AudioPacket pkt;
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    // head = 37, so the oldest intact sequence is 37 - 32 + 1 = 6 (batch #5).
    EXPECT_EQ(last, (uint32_t)(total - AUDIO_RING_SLOTS + 1));
    EXPECT_EQ(pkt.seq, (uint16_t)(total - static_cast<int>(AUDIO_RING_SLOTS)));

    TEST("after the skip the remaining batches drain in order to the newest");
    uint16_t expected = static_cast<uint16_t>(pkt.seq + 1);
    while (readNextAudioSlot(sec.data(), last, pkt)) {
        EXPECT_EQ(pkt.seq, expected);
        expected++;
    }
    EXPECT_EQ(last, (uint32_t)total);
}

// The producer zeroes a slot's SeqNo before touching its payload, so a reader
// arriving mid-write sees a value matching no expectation. Reproduced exactly:
// reserve the sequence, mark the slot in progress, half-overwrite the payload,
// and assert the reader hands back nothing rather than a half-old batch.
static void test_torn_write_rejected() {
    TEST("a half-written slot is refused, so no batch is ever returned torn");
    std::vector<uint8_t> sec = freshSection();
    const std::vector<int16_t> oldPcm(8, 42);
    EXPECT(writeAudioSlot(sec.data(), 2, 0, oldPcm.data(), 8));

    uint32_t last = 0;
    AudioPacket pkt;
    EXPECT(readNextAudioSlot(sec.data(), last, pkt)); // consume the clean batch
    EXPECT_EQ(last, (uint32_t)1);

    // Now replay a write that stops halfway: head reserved, SeqNo zeroed,
    // header written, and only the first half of the payload copied in.
    uint8_t* slot = slotAt(sec, 2);
    storeU32(sec.data(), 2);
    storeU32(slot + AUDIO_SLOT_SEQNO_OFFSET, 0);
    storeU32(slot + AUDIO_SLOT_SERIAL_OFFSET, 2);
    const uint16_t seq = 1, samples = 8;
    std::memcpy(slot + AUDIO_SLOT_SEQ_OFFSET, &seq, sizeof(seq));
    std::memcpy(slot + AUDIO_SLOT_SAMPLES_OFFSET, &samples, sizeof(samples));
    const std::vector<int16_t> halfNew(4, -1);
    std::memcpy(slot + AUDIO_SLOT_DATA_OFFSET, halfNew.data(), halfNew.size() * sizeof(int16_t));

    EXPECT(!readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(last, (uint32_t)1);        // cursor stayed put; nothing was consumed
    EXPECT_EQ(pkt.data[0], (int16_t)42); // and the caller's buffer is untouched

    TEST("the batch becomes readable, whole, once the producer publishes it");
    const std::vector<int16_t> newPcm(8, -1);
    std::memcpy(slot + AUDIO_SLOT_DATA_OFFSET, newPcm.data(), newPcm.size() * sizeof(int16_t));
    storeU32(slot + AUDIO_SLOT_SEQNO_OFFSET, 2);
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(last, (uint32_t)2);
    EXPECT_EQ(pkt.sampleCount, (uint16_t)8);
    EXPECT_EQ(pkt.data[0], (int16_t)-1);
    EXPECT_EQ(pkt.data[7], (int16_t)-1);
}

// The other half of the same guard: a slot stamped with a sequence that is not
// the one the cursor expects belongs to a different lap, and is refused rather
// than replayed as if it were the next batch.
static void test_foreign_sequence_rejected() {
    TEST("a slot carrying someone else's sequence is not consumed");
    std::vector<uint8_t> sec = freshSection();
    const std::vector<int16_t> pcm(8, 7);
    EXPECT(writeAudioSlot(sec.data(), 4, 0, pcm.data(), 8));

    uint8_t* slot = slotAt(sec, 1);
    storeU32(slot + AUDIO_SLOT_SEQNO_OFFSET, 99);
    uint32_t last = 0;
    AudioPacket pkt;
    EXPECT(!readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(last, (uint32_t)0);
}

static void test_head_advances_and_wraps_slots() {
    TEST("the head counter advances once per batch and reuses slots modulo 32");
    std::vector<uint8_t> sec = freshSection();
    const std::vector<int16_t> pcm(2, 1);
    for (uint32_t i = 1; i <= AUDIO_RING_SLOTS + 1; ++i) {
        EXPECT(writeAudioSlot(sec.data(), 1, static_cast<uint16_t>(i), pcm.data(), 2));
        EXPECT_EQ(audioRingHead(sec.data()), i);
    }
    // Batch 33 lands back in slot 0, whose SeqNo now reads 33.
    EXPECT_EQ(u32at(slotAt(sec, AUDIO_RING_SLOTS + 1), AUDIO_SLOT_SEQNO_OFFSET),
              (uint32_t)(AUDIO_RING_SLOTS + 1));
    EXPECT_EQ(slotAt(sec, 1), slotAt(sec, AUDIO_RING_SLOTS + 1));
}

// The per-stream sequence is a u16 that wraps; it is the consumer's gap
// signal, never the ring cursor, so a wrap must be invisible to the drain.
static void test_stream_sequence_wraps() {
    TEST("the per-stream u16 seq wraps through 0xFFFF without disturbing the drain");
    std::vector<uint8_t> sec = freshSection();
    const std::vector<int16_t> pcm(2, 3);
    EXPECT(writeAudioSlot(sec.data(), 1, 0xFFFE, pcm.data(), 2));
    EXPECT(writeAudioSlot(sec.data(), 1, 0xFFFF, pcm.data(), 2));
    EXPECT(writeAudioSlot(sec.data(), 1, 0x0000, pcm.data(), 2));

    uint32_t last = 0;
    AudioPacket pkt;
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(pkt.seq, (uint16_t)0xFFFE);
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(pkt.seq, (uint16_t)0xFFFF);
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(pkt.seq, (uint16_t)0x0000);
    EXPECT_EQ(last, (uint32_t)3);
}

static void test_serial_is_carried_for_the_reader_to_check() {
    TEST("each batch carries the serial it belongs to, so an aliased ring is detectable");
    std::vector<uint8_t> sec = freshSection();
    const std::vector<int16_t> pcm(2, 1);
    EXPECT(writeAudioSlot(sec.data(), 1, 0, pcm.data(), 2));
    EXPECT(writeAudioSlot(sec.data(), 8, 1, pcm.data(), 2));

    uint32_t last = 0;
    AudioPacket pkt;
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(pkt.serial, (uint32_t)1);
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(pkt.serial, (uint32_t)8);
}

static void test_oversized_sample_count_is_clamped() {
    TEST("a slot claiming more samples than fit is clamped, never over-read");
    std::vector<uint8_t> sec = freshSection();
    const std::vector<int16_t> pcm(8, 9);
    EXPECT(writeAudioSlot(sec.data(), 1, 0, pcm.data(), 8));

    // A corrupt or hostile producer stamping an impossible count.
    uint8_t* slot = slotAt(sec, 1);
    const uint16_t bogus = 0xFFFF;
    std::memcpy(slot + AUDIO_SLOT_SAMPLES_OFFSET, &bogus, sizeof(bogus));

    uint32_t last = 0;
    AudioPacket pkt;
    EXPECT(readNextAudioSlot(sec.data(), last, pkt));
    EXPECT_EQ(pkt.sampleCount, (uint16_t)AUDIO_SLOT_SAMPLE_CAPACITY);
}

int main() {
    test_layout_offsets();
    test_write_read_round_trip();
    test_negative_samples_survive();
    test_in_order_stream();
    test_capacity_guard();
    test_lap_skip();
    test_torn_write_rejected();
    test_foreign_sequence_rejected();
    test_head_advances_and_wraps_slots();
    test_stream_sequence_wraps();
    test_serial_is_carried_for_the_reader_to_check();
    test_oversized_sample_count_is_clamped();

    std::cout << "hidmaestro_audio_wire: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
