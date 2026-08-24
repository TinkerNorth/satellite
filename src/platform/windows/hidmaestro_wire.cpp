// SPDX-License-Identifier: LGPL-3.0-or-later
#include "hidmaestro_wire.h"

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

// Single-writer seqlock: bump to odd, write the payload, bump to even. A
// reader sampling mid-write sees an odd or changed counter and retries. The
// release fence orders the payload stores before the completing even store;
// the cross-process reader pairs it with an acquire load of the counter.
void beginWrite(uint8_t* section, uint32_t seq) {
    storeU32(section + INPUT_SEQNO_OFFSET, seq + 1);
    std::atomic_thread_fence(std::memory_order_release);
}

void endWrite(uint8_t* section, uint32_t seq) {
    std::atomic_thread_fence(std::memory_order_release);
    storeU32(section + INPUT_SEQNO_OFFSET, seq + 2);
}

} // namespace

uint32_t readSeqNo(const uint8_t* section) { return loadU32(section + INPUT_SEQNO_OFFSET); }

bool writeInputFrame(uint8_t* section, const uint8_t* report, uint16_t reportLen,
                     const uint8_t* gip) {
    if (reportLen > INPUT_DATA_CAPACITY) return false;

    const uint32_t seq = loadU32(section + INPUT_SEQNO_OFFSET);
    beginWrite(section, seq);

    storeU32(section + INPUT_DATASIZE_OFFSET, reportLen);
    if (reportLen > 0) std::memcpy(section + INPUT_DATA_OFFSET, report, reportLen);
    if (gip != nullptr) std::memcpy(section + INPUT_GIP_OFFSET, gip, INPUT_GIP_LENGTH);
    storeU32(section + INPUT_EXT_SIZE_OFFSET, 0);

    endWrite(section, seq);
    return true;
}

bool writeExtendedInputFrame(uint8_t* section, const uint8_t* report, uint16_t reportLen) {
    if (reportLen > INPUT_EXT_DATA_CAPACITY) return false;

    const uint32_t seq = loadU32(section + INPUT_SEQNO_OFFSET);
    beginWrite(section, seq);

    storeU32(section + INPUT_EXT_SIZE_OFFSET, reportLen);
    if (reportLen > 0) std::memcpy(section + INPUT_EXT_DATA_OFFSET, report, reportLen);

    endWrite(section, seq);
    return true;
}

bool readNextOutputPacket(const uint8_t* section, uint32_t& lastSeq, OutputPacket& out) {
    const uint32_t head = loadU32(section);
    if (head == lastSeq) return false;

    uint32_t nextSeq = lastSeq + 1;
    if (head > nextSeq + static_cast<uint32_t>(OUTPUT_RING_SLOTS) - 1)
        nextSeq = head - static_cast<uint32_t>(OUTPUT_RING_SLOTS) + 1;

    const size_t slotBase =
        OUTPUT_HEADER_SIZE +
        static_cast<size_t>((nextSeq - 1) % OUTPUT_RING_SLOTS) * OUTPUT_SLOT_SIZE;
    const uint8_t* slot = section + slotBase;

    for (int retries = 0; retries < 4; ++retries) {
        const uint32_t seqBefore = loadU32(slot + OUTPUT_SLOT_SEQNO_OFFSET);
        // Producers reserve their sequence via an interlocked Head increment
        // and publish the slot's SeqNo last, so a mismatch here is a reserved
        // slot whose write hasn't finished (its doorbell follows the publish;
        // report no-new-data and retry on the next wake) or a 64-lap
        // overwrite (handled by the skip-ahead above).
        if (seqBefore != nextSeq) return false;
        std::atomic_thread_fence(std::memory_order_acquire);

        out.source = slot[OUTPUT_SLOT_SOURCE_OFFSET];
        out.reportId = slot[OUTPUT_SLOT_REPORT_ID_OFFSET];
        uint16_t sz = loadU16(slot + OUTPUT_SLOT_SIZE_OFFSET);
        if (sz > OUTPUT_SLOT_DATA_CAPACITY) sz = OUTPUT_SLOT_DATA_CAPACITY;
        out.size = sz;
        if (sz > 0) std::memcpy(out.data, slot + OUTPUT_SLOT_DATA_OFFSET, sz);

        std::atomic_thread_fence(std::memory_order_acquire);
        if (loadU32(slot + OUTPUT_SLOT_SEQNO_OFFSET) == seqBefore) {
            lastSeq = nextSeq;
            return true;
        }
    }
    return false;
}

} // namespace hidmaestro
} // namespace satellite
