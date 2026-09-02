// SPDX-License-Identifier: LGPL-3.0-or-later

// AudioJitterWindow (core/audio/audio_jitter.h): the 2-frame reorder window
// that decides, from wrapping u16 sequence numbers alone, which frames to play,
// which to conceal, and which arrived too late to be worth anything.
//
// Dependency-free by design, so this suite links nothing: it can run on a lane
// with no codec at all, which is the point of keeping the ordering rules out of
// the codec.
#include "../src/core/audio/audio_jitter.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "test_util.h"

namespace {

using Kind = AudioJitterWindow::Event::Kind;
using Accept = AudioJitterWindow::Accept;

// Packets are only ever compared by identity here, so each one is a block of a
// distinctive byte: it makes "the window handed back the packet I pushed" an
// assertion rather than a hope.
std::vector<uint8_t> packet(uint8_t tag, size_t bytes = 24) {
    return std::vector<uint8_t>(bytes, tag);
}

AudioJitterWindow::Result push(AudioJitterWindow& w, uint16_t seq, const std::vector<uint8_t>& p) {
    return w.push(seq, p.data(), p.size());
}

// One-line shape of a result: "P12" for a packet, "G12" for a gap, "G12/F" for
// a gap that has an FEC carrier in hand. Comparing shapes catches ordering
// mistakes that per-field asserts miss.
std::string shape(const AudioJitterWindow::Result& r) {
    std::string s;
    for (int i = 0; i < r.count; i++) {
        const AudioJitterWindow::Event& e = r.events[i];
        if (!s.empty()) s += " ";
        s += (e.kind == Kind::Packet ? "P" : "G");
        s += std::to_string(e.seq);
        if (e.kind == Kind::Gap && e.fecCarrier != nullptr) s += "/F";
    }
    return s;
}

// Every byte of the event's payload equals `tag`.
bool payloadIs(const AudioJitterWindow::Event& e, uint8_t tag) {
    if (e.data == nullptr || e.len == 0) return false;
    for (size_t i = 0; i < e.len; i++) {
        if (e.data[i] != tag) return false;
    }
    return true;
}

bool carrierIs(const AudioJitterWindow::Event& e, uint8_t tag) {
    if (e.fecCarrier == nullptr || e.fecCarrierLen == 0) return false;
    for (size_t i = 0; i < e.fecCarrierLen; i++) {
        if (e.fecCarrier[i] != tag) return false;
    }
    return true;
}

void test_inOrder_passesThrough() {
    TEST("jitter: an in-order stream passes straight through, one packet in one out");
    AudioJitterWindow w;
    EXPECT(!w.primed());

    for (int i = 0; i < 8; i++) {
        const auto p = packet(static_cast<uint8_t>(0x40 + i));
        const auto r = push(w, static_cast<uint16_t>(100 + i), p);
        EXPECT(r.accept == Accept::Ok);
        EXPECT_EQ(r.count, 1);
        EXPECT(r.events[0].kind == Kind::Packet);
        EXPECT_EQ((int)r.events[0].seq, 100 + i);
        EXPECT(payloadIs(r.events[0], static_cast<uint8_t>(0x40 + i)));
        // The clean path never copies: the event aliases the caller's buffer.
        EXPECT(r.events[0].data == p.data());
        EXPECT_EQ(w.buffered(), 0);
    }
    EXPECT(w.primed());
    EXPECT_EQ((int)w.nextSeq(), 108);
}

void test_firstPacketPrimes_anySeq() {
    TEST("jitter: the first packet defines the origin, whatever its seq");
    AudioJitterWindow w;
    const auto p = packet(0x11);
    // There is no such thing as a late or missing frame before the stream
    // started, so a mid-range opening seq is simply where it starts.
    const auto r = push(w, 40000, p);
    EXPECT(r.accept == Accept::Ok);
    EXPECT_EQ(shape(r), std::string("P40000"));
    EXPECT_EQ((int)w.nextSeq(), 40001);
}

void test_singleReorder_heals() {
    TEST("jitter: one swapped pair is reordered, not concealed");
    AudioJitterWindow w;
    push(w, 10, packet(0xA0));
    push(w, 11, packet(0xA1));

    // 13 before 12: held, nothing due yet. This is the whole reason the window
    // exists, so it must not emit a gap here.
    const auto p13 = packet(0xA3);
    const auto held = push(w, 13, p13);
    EXPECT(held.accept == Accept::Ok);
    EXPECT_EQ(held.count, 0);
    EXPECT_EQ(w.buffered(), 1);

    // 12 arrives: it goes out, and 13 follows it immediately.
    const auto p12 = packet(0xA2);
    const auto healed = push(w, 12, p12);
    EXPECT(healed.accept == Accept::Ok);
    EXPECT_EQ(shape(healed), std::string("P12 P13"));
    EXPECT(payloadIs(healed.events[0], 0xA2));
    EXPECT(payloadIs(healed.events[1], 0xA3));
    EXPECT_EQ(w.buffered(), 0);
    EXPECT_EQ((int)w.nextSeq(), 14);
}

void test_lostFrame_becomesGapWithFecCarrier() {
    TEST("jitter: a lost frame is declared once something 2 ahead lands, with its carrier");
    AudioJitterWindow w;
    push(w, 10, packet(0xB0));
    push(w, 11, packet(0xB1));

    // 12 is lost. 13 alone proves nothing (it could be a reorder).
    const auto p13 = packet(0xB3);
    EXPECT_EQ(push(w, 13, p13).count, 0);

    // 14 is 2 ahead of the frame we want, which is the window's whole
    // definition of "lost". The gap names 12 and carries 13, because Opus hides
    // a redundant copy of 12 inside 13 and that is what makes recovery
    // possible; then 13 and 14 follow in order.
    const auto p14 = packet(0xB4);
    const auto r = push(w, 14, p14);
    EXPECT(r.accept == Accept::Ok);
    EXPECT_EQ(shape(r), std::string("G12/F P13 P14"));
    EXPECT(carrierIs(r.events[0], 0xB3));
    EXPECT(r.events[0].data == nullptr); // a gap has no packet of its own
    EXPECT(payloadIs(r.events[1], 0xB3));
    EXPECT(payloadIs(r.events[2], 0xB4));
}

void test_twoLostFrames_concealThenRecover() {
    TEST("jitter: back-to-back losses conceal the first blind, recover the second by FEC");
    AudioJitterWindow w;
    push(w, 10, packet(0xC0));
    push(w, 11, packet(0xC1));

    // 12 and 13 both lost. When 14 lands it is 2 ahead of 12, so 12 is
    // declared; nothing carries it (13 never arrived), so the gap is blind.
    const auto p14 = packet(0xC4);
    const auto first = push(w, 14, p14);
    EXPECT_EQ(shape(first), std::string("G12"));
    EXPECT(first.events[0].fecCarrier == nullptr);

    // 15 lands: now 13 is 2 behind, and 14 IS in hand to carry it.
    const auto p15 = packet(0xC5);
    const auto second = push(w, 15, p15);
    EXPECT_EQ(shape(second), std::string("G13/F P14 P15"));
    EXPECT(carrierIs(second.events[0], 0xC4));
}

void test_lateFrame_isDropped() {
    TEST("jitter: a frame that arrives after its slot played is dropped, not spliced in");
    AudioJitterWindow w;
    push(w, 10, packet(0xD0));
    push(w, 11, packet(0xD1));
    push(w, 13, packet(0xD3));
    const auto flushed = push(w, 14, packet(0xD4));
    EXPECT_EQ(shape(flushed), std::string("G12/F P13 P14"));

    // 12 finally shows up, long after it was concealed. Playing it now would be
    // an audible jump backwards.
    const auto late = push(w, 12, packet(0xD2));
    EXPECT(late.accept == Accept::Late);
    EXPECT_EQ(late.count, 0);

    // So would replaying a frame already emitted.
    const auto replay = push(w, 13, packet(0xD3));
    EXPECT(replay.accept == Accept::Late);
    EXPECT_EQ(replay.count, 0);

    // The stream carries on untouched.
    const auto next = push(w, 15, packet(0xD5));
    EXPECT_EQ(shape(next), std::string("P15"));
}

void test_duplicateOfHeldFrame_isDropped() {
    TEST("jitter: a duplicate of a frame already waiting is dropped");
    AudioJitterWindow w;
    push(w, 10, packet(0xE0));
    EXPECT_EQ(push(w, 12, packet(0xE2)).count, 0); // held, waiting on 11

    const auto dup = push(w, 12, packet(0xE2));
    EXPECT(dup.accept == Accept::Duplicate);
    EXPECT_EQ(dup.count, 0);
    EXPECT_EQ(w.buffered(), 1);

    // The real 11 still heals the hole.
    EXPECT_EQ(shape(push(w, 11, packet(0xE1))), std::string("P11 P12"));
}

void test_malformedPackets_rejected() {
    TEST("jitter: empty, null and oversize packets are refused without touching the stream");
    AudioJitterWindow w;
    push(w, 10, packet(0xF0));

    const auto p = packet(0xF1);
    EXPECT(w.push(11, nullptr, p.size()).accept == Accept::Rejected);
    EXPECT(w.push(11, p.data(), 0).accept == Accept::Rejected);

    const auto huge = packet(0xF2, AUDIO_JITTER_MAX_PACKET_BYTES + 1);
    EXPECT(w.push(11, huge.data(), huge.size()).accept == Accept::Rejected);

    // Exactly at the ceiling is fine; the bound is on what the window can be
    // made to allocate, not on what is plausible.
    const auto atCap = packet(0xF3, AUDIO_JITTER_MAX_PACKET_BYTES);
    const auto ok = w.push(11, atCap.data(), atCap.size());
    EXPECT(ok.accept == Accept::Ok);
    EXPECT_EQ(shape(ok), std::string("P11"));

    // None of the refusals moved the stream on.
    EXPECT_EQ((int)w.nextSeq(), 12);
}

void test_wrap_inOrderAcrossZero() {
    TEST("jitter: 0x0000 is the frame after 0xFFFF, not 65536 frames of loss");
    AudioJitterWindow w;
    const uint16_t start = 0xFFFD;
    for (int i = 0; i < 6; i++) {
        const uint16_t seq = static_cast<uint16_t>(start + i);
        const auto r = push(w, seq, packet(static_cast<uint8_t>(0x50 + i)));
        EXPECT(r.accept == Accept::Ok);
        EXPECT_EQ(r.count, 1);
        EXPECT_EQ((int)r.events[0].seq, (int)seq);
    }
    EXPECT_EQ((int)w.nextSeq(), 3);
}

void test_wrap_reorderAndGapAcrossZero() {
    TEST("jitter: reorder and gap detection keep working across the wrap");
    AudioJitterWindow w;
    push(w, 0xFFFD, packet(0x60));
    push(w, 0xFFFE, packet(0x61));

    // 0x0000 before 0xFFFF: a one-frame reorder that happens to straddle the
    // wrap. Signed u16 arithmetic is what keeps this from reading as a jump.
    EXPECT_EQ(push(w, 0x0000, packet(0x63)).count, 0);
    const auto healed = push(w, 0xFFFF, packet(0x62));
    EXPECT_EQ(shape(healed), std::string("P65535 P0"));

    // And a loss straddling it: 0x0001 lost, 0x0002 held, 0x0003 declares it.
    EXPECT_EQ(push(w, 0x0002, packet(0x65)).count, 0);
    const auto gapped = push(w, 0x0003, packet(0x66));
    EXPECT_EQ(shape(gapped), std::string("G1/F P2 P3"));
    EXPECT(carrierIs(gapped.events[0], 0x65));
}

void test_longDropout_concealsThenResyncs() {
    TEST("jitter: a long dropout is concealed only to the cap, then the window resyncs");
    AudioJitterWindow w;
    push(w, 10, packet(0x70));

    // Half a second of nothing, then the stream resumes. Concealing all of it
    // would inject synthetic audio AND hold the stream that far behind live.
    const auto resume = push(w, 36, packet(0x71));
    int gaps = 0;
    int packets = 0;
    for (int i = 0; i < resume.count; i++) {
        if (resume.events[i].kind == Kind::Gap)
            gaps++;
        else
            packets++;
    }
    EXPECT_EQ(gaps, AUDIO_JITTER_MAX_CONCEAL_FRAMES);
    EXPECT_EQ(packets, 1);
    EXPECT_EQ(shape(resume), std::string("G11 G12 P36"));
    // Resynchronised onto the new audio, not still grinding through the hole.
    EXPECT_EQ((int)w.nextSeq(), 37);
    EXPECT_EQ(w.buffered(), 0);

    // A frame from inside the skipped range is history now.
    EXPECT(push(w, 20, packet(0x72)).accept == Accept::Late);
}

void test_concealRunResetsAfterGoodFrame() {
    TEST("jitter: the concealment budget refills once real audio gets through");
    AudioJitterWindow w;
    push(w, 10, packet(0x80));
    // Burn the budget on one dropout...
    const auto first = push(w, 30, packet(0x81));
    EXPECT_EQ(shape(first), std::string("G11 G12 P30"));
    // ...then a clean frame, then another dropout: the second one gets its own
    // full allowance rather than inheriting a spent counter.
    EXPECT_EQ(shape(push(w, 31, packet(0x82))), std::string("P31"));
    EXPECT_EQ(shape(push(w, 60, packet(0x83))), std::string("G32 G33 P60"));
}

void test_windowNeverHoldsMoreThanOneFrame() {
    TEST("jitter: whatever the arrival order, at most one frame is left waiting");
    AudioJitterWindow w;
    // A deliberately hostile order: reorders, repeats, jumps forward and back.
    const uint16_t order[] = {100, 102, 101, 105, 103,   104, 104, 110, 108,
                              111, 112, 109, 113, 65535, 116, 115, 117};
    for (uint16_t seq : order) {
        push(w, seq, packet(0x90));
        // The slots_ array is sized by this invariant; if it ever stops
        // holding, the window would be writing past its storage.
        EXPECT(w.buffered() < AUDIO_JITTER_WINDOW_FRAMES);
    }
}

void test_eventsNeverOverrunTheResultArray() {
    TEST("jitter: no arrival order fills more events than one result can carry");
    AudioJitterWindow w;
    // Worst shape the window can produce: something held, then a jump big
    // enough to burn the concealment budget and resync in one push.
    push(w, 10, packet(0xA0));
    push(w, 11, packet(0xA1));
    EXPECT_EQ(push(w, 13, packet(0xA3)).count, 0); // 12 held back
    const auto burst = push(w, 900, packet(0xA9));
    EXPECT(burst.count <= AUDIO_JITTER_MAX_EVENTS_PER_PUSH);
    EXPECT(burst.count > 0);
}

void test_reset_startsAFreshStream() {
    TEST("jitter: reset forgets the old pad's stream entirely");
    AudioJitterWindow w;
    push(w, 500, packet(0xB0));
    push(w, 502, packet(0xB2)); // left waiting
    EXPECT_EQ(w.buffered(), 1);

    w.reset();
    EXPECT(!w.primed());
    EXPECT_EQ(w.buffered(), 0);

    // A seq that would have been ancient history before the reset is now
    // simply where the new stream begins: a replugged pad restarts its
    // numbering and must not have its first second thrown away as "late".
    const auto r = push(w, 3, packet(0xB3));
    EXPECT(r.accept == Accept::Ok);
    EXPECT_EQ(shape(r), std::string("P3"));
}

} // namespace

int main() {
    std::cout << "Running audio jitter-window tests...\n\n";
    test_inOrder_passesThrough();
    test_firstPacketPrimes_anySeq();
    test_singleReorder_heals();
    test_lostFrame_becomesGapWithFecCarrier();
    test_twoLostFrames_concealThenRecover();
    test_lateFrame_isDropped();
    test_duplicateOfHeldFrame_isDropped();
    test_malformedPackets_rejected();
    test_wrap_inOrderAcrossZero();
    test_wrap_reorderAndGapAcrossZero();
    test_longDropout_concealsThenResyncs();
    test_concealRunResetsAfterGoodFrame();
    test_windowNeverHoldsMoreThanOneFrame();
    test_eventsNeverOverrunTheResultArray();
    test_reset_startsAFreshStream();

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
