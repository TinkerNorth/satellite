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

void storeU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }

// Single-writer seqlock: bump to odd, write the payload, bump to even. A reader
// sampling mid-write sees an odd or changed counter and retries. The release
// fence orders the payload stores before the completing even store; the cross-
// process reader pairs it with an acquire load of the counter.
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

bool writeInputFrame(uint8_t* section, const uint8_t* report, uint16_t reportLen) {
    if (reportLen > INPUT_DATA_CAPACITY) return false;

    const uint32_t seq = loadU32(section + INPUT_SEQNO_OFFSET);
    beginWrite(section, seq);

    storeU32(section + INPUT_DATASIZE_OFFSET, reportLen);
    if (reportLen > 0) std::memcpy(section + INPUT_DATA_OFFSET, report, reportLen);
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

} // namespace hidmaestro
} // namespace satellite
