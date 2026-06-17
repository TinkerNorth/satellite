// SPDX-License-Identifier: LGPL-3.0-or-later

// HIDMaestro input-section wire packer. The UMDF2 driver and its writer share a
// per-controller named shared-memory section; the hot path publishes input
// frames into it with a seqlock so the driver never reads a torn frame — no
// managed code on the per-frame path. Offsets below are the contract with
// driver/driver.h (HIDMAESTRO_SHARED_INPUT, 362 bytes as of HIDMaestro v1.3.5).
//
// Deliberately free of <windows.h>: the packing is pure byte/atomic work, so it
// links into the portable test target and is verified on every CI platform. The
// elevated section *creation* is the provisioner's job (hidmaestro_provisioner.h).
#pragma once

#include <cstddef>
#include <cstdint>

namespace satellite {
namespace hidmaestro {

inline constexpr size_t INPUT_SEQNO_OFFSET = 0;    // ULONG seqlock counter
inline constexpr size_t INPUT_DATASIZE_OFFSET = 4; // ULONG legacy report length
inline constexpr size_t INPUT_DATA_OFFSET = 8;     // legacy report data, RID stripped
inline constexpr size_t INPUT_DATA_CAPACITY = 256;
inline constexpr size_t INPUT_GIP_OFFSET = 264; // UCHAR GipData[14]
inline constexpr size_t INPUT_GIP_LENGTH = 14;
inline constexpr size_t INPUT_EXT_SIZE_OFFSET = 278; // ULONG: 0 = legacy, >0 = extended
inline constexpr size_t INPUT_EXT_DATA_OFFSET = 282; // full RID-included report (Sony BT 0x31/0x11)
inline constexpr size_t INPUT_EXT_DATA_CAPACITY = 80;
inline constexpr size_t INPUT_SECTION_SIZE = 362;

// Current seqlock counter (even = stable frame, odd = write in progress).
uint32_t readSeqNo(const uint8_t* section);

// Publish a legacy input frame (report data with the Report ID stripped) under
// the seqlock. Clears ExtendedReportSize so the driver can't reuse stale
// extended bytes from a prior arming. Returns false (no write) if reportLen
// exceeds INPUT_DATA_CAPACITY.
bool writeInputFrame(uint8_t* section, const uint8_t* report, uint16_t reportLen);

// Publish an extended input frame (full RID-included report, e.g. Sony BT 0x31)
// under the seqlock, setting ExtendedReportSize so the driver takes the extended
// path. Returns false if reportLen exceeds INPUT_EXT_DATA_CAPACITY.
bool writeExtendedInputFrame(uint8_t* section, const uint8_t* report, uint16_t reportLen);

} // namespace hidmaestro
} // namespace satellite
