// SPDX-License-Identifier: LGPL-3.0-or-later
#include "hidmaestro_audio_wire.h"

#include <atomic>
#include <cstring>

namespace satellite {
namespace hidmaestro {

namespace {

uint32_t loadU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint16_t loadU16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void storeU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
void storeU16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof(v)); }

} // namespace

uint32_t audioRingHead(const uint8_t* section) { return loadU32(section); }

bool writeAudioSlot(uint8_t* section, uint32_t serial, uint16_t seq, const int16_t* pcm,
                    uint16_t sampleCount) {
    if (sampleCount > AUDIO_SLOT_SAMPLE_CAPACITY) return false;

    // Reserve first, so a reader that wakes early sees the slot as claimed and
    // unpublished rather than as the previous lap's still-valid batch.
    const uint32_t next = loadU32(section) + 1;
    storeU32(section, next);

    uint8_t* slot = section + AUDIO_RING_HEADER_SIZE +
                    static_cast<size_t>((next - 1) % AUDIO_RING_SLOTS) * AUDIO_SLOT_SIZE;

    // Zero marks "in progress": no published batch ever carries sequence 0, so
    // a reader mid-write reads a value that matches no expectation and retries.
    storeU32(slot + AUDIO_SLOT_SEQNO_OFFSET, 0);
    std::atomic_thread_fence(std::memory_order_release);

    storeU32(slot + AUDIO_SLOT_SERIAL_OFFSET, serial);
    storeU16(slot + AUDIO_SLOT_SEQ_OFFSET, seq);
    storeU16(slot + AUDIO_SLOT_SAMPLES_OFFSET, sampleCount);
    if (sampleCount > 0) {
        std::memcpy(slot + AUDIO_SLOT_DATA_OFFSET, pcm,
                    static_cast<size_t>(sampleCount) * sizeof(int16_t));
    }

    std::atomic_thread_fence(std::memory_order_release);
    storeU32(slot + AUDIO_SLOT_SEQNO_OFFSET, next);
    return true;
}

bool readNextAudioSlot(const uint8_t* section, uint32_t& lastSeq, AudioPacket& out) {
    const uint32_t head = loadU32(section);
    if (head == lastSeq) return false;

    uint32_t nextSeq = lastSeq + 1;
    if (head > nextSeq + static_cast<uint32_t>(AUDIO_RING_SLOTS) - 1)
        nextSeq = head - static_cast<uint32_t>(AUDIO_RING_SLOTS) + 1;

    const uint8_t* slot = section + AUDIO_RING_HEADER_SIZE +
                          static_cast<size_t>((nextSeq - 1) % AUDIO_RING_SLOTS) * AUDIO_SLOT_SIZE;

    for (int retries = 0; retries < 4; ++retries) {
        const uint32_t seqBefore = loadU32(slot + AUDIO_SLOT_SEQNO_OFFSET);
        // Not our sequence: either the producer reserved this slot and has not
        // published yet (its doorbell follows the publish, so the next wake
        // gets it) or it lapped us, which the skip-ahead above already handled.
        if (seqBefore != nextSeq) return false;
        std::atomic_thread_fence(std::memory_order_acquire);

        out.serial = loadU32(slot + AUDIO_SLOT_SERIAL_OFFSET);
        out.seq = loadU16(slot + AUDIO_SLOT_SEQ_OFFSET);
        uint16_t samples = loadU16(slot + AUDIO_SLOT_SAMPLES_OFFSET);
        if (samples > AUDIO_SLOT_SAMPLE_CAPACITY)
            samples = static_cast<uint16_t>(AUDIO_SLOT_SAMPLE_CAPACITY);
        out.sampleCount = samples;
        if (samples > 0) {
            std::memcpy(out.data, slot + AUDIO_SLOT_DATA_OFFSET,
                        static_cast<size_t>(samples) * sizeof(int16_t));
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        if (loadU32(slot + AUDIO_SLOT_SEQNO_OFFSET) == seqBefore) {
            lastSeq = nextSeq;
            return true;
        }
    }
    return false;
}

} // namespace hidmaestro
} // namespace satellite
