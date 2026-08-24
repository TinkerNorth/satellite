// SPDX-License-Identifier: LGPL-3.0-or-later

// HIDMaestro shared-memory wire codec. The UMDF2 driver (and its XUSB
// companion) share per-controller named sections with their writer; the hot
// path publishes input frames into the input section with a seqlock so the
// driver never reads a torn frame, and the rumble/lightbar return path drains
// the driver's output ring — no managed code on any per-frame path. Offsets
// are the contract with HIDMaestro driver/driver.h at v1.7.0 (HIDMAESTRO_
// SHARED_INPUT 362 bytes, HIDMAESTRO_SHARED_OUTPUT 8 + 64*264 bytes); the
// protocol carries no version field, so the section SIZES are the layout
// check (a pre-v1.3.5 278-byte input section fails the size probe).
//
// Deliberately free of <windows.h>: the packing is pure byte/atomic work, so
// it links into the portable test target and is verified on every CI platform.
// Section names, handles, and the elevated section creation are the
// provisioner's job (hidmaestro_provisioner.h).
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
inline constexpr size_t INPUT_EXT_DATA_OFFSET = 282; // full RID-included report (Sony BT)
inline constexpr size_t INPUT_EXT_DATA_CAPACITY = 80;
inline constexpr size_t INPUT_SECTION_SIZE = 362;

inline constexpr size_t OUTPUT_RING_SLOTS = 64;
inline constexpr size_t OUTPUT_HEADER_SIZE = 8; // Head u32 @0, reserved u32 @4
inline constexpr size_t OUTPUT_SLOT_SEQNO_OFFSET = 0;
inline constexpr size_t OUTPUT_SLOT_SOURCE_OFFSET = 4;
inline constexpr size_t OUTPUT_SLOT_REPORT_ID_OFFSET = 5;
inline constexpr size_t OUTPUT_SLOT_SIZE_OFFSET = 6; // u16
inline constexpr size_t OUTPUT_SLOT_DATA_OFFSET = 8;
inline constexpr size_t OUTPUT_SLOT_DATA_CAPACITY = 256;
inline constexpr size_t OUTPUT_SLOT_SIZE = OUTPUT_SLOT_DATA_OFFSET + OUTPUT_SLOT_DATA_CAPACITY;
inline constexpr size_t OUTPUT_SECTION_SIZE =
    OUTPUT_HEADER_SIZE + OUTPUT_RING_SLOTS * OUTPUT_SLOT_SIZE; // 16904

// Ring-slot Source values (HIDMaestro driver.h HIDMAESTRO_OUTPUT_SOURCE_*).
// Sources HID_OUTPUT/HID_FEATURE carry the report id in the slot's ReportId
// field with the RID stripped from Data; XINPUT carries the raw XUSB
// SET_STATE buffer (motors at Data[2]/Data[3]) with ReportId always 0.
inline constexpr uint8_t OUTPUT_SOURCE_HID_OUTPUT = 0;
inline constexpr uint8_t OUTPUT_SOURCE_HID_FEATURE = 1;
inline constexpr uint8_t OUTPUT_SOURCE_XINPUT = 2;
inline constexpr uint8_t OUTPUT_SOURCE_HID_FEATURE_READ = 3;

// Current input seqlock counter (even = stable frame, odd = write in progress).
uint32_t readSeqNo(const uint8_t* section);

// Publish a legacy input frame (report data with the Report ID stripped) under
// the seqlock. `gip`, when non-null, is the 14-byte XUSB companion slice
// (Xbox-VID profiles only). Always clears ExtendedReportSize so the driver
// can't reuse stale extended bytes from a prior arming. Returns false (no
// write) if reportLen exceeds INPUT_DATA_CAPACITY.
bool writeInputFrame(uint8_t* section, const uint8_t* report, uint16_t reportLen,
                     const uint8_t* gip = nullptr);

// Publish an extended input frame (full RID-included report, e.g. Sony BT
// 0x31) under the seqlock, setting ExtendedReportSize so the driver takes the
// extended path. Returns false if reportLen exceeds INPUT_EXT_DATA_CAPACITY.
bool writeExtendedInputFrame(uint8_t* section, const uint8_t* report, uint16_t reportLen);

struct OutputPacket {
    uint8_t source = 0;
    uint8_t reportId = 0;
    uint16_t size = 0;
    uint8_t data[OUTPUT_SLOT_DATA_CAPACITY] = {};
};

// Drain step over the driver's output ring: copies the packet at lastSeq+1
// when one is published and advances lastSeq. A reader more than 64 packets
// behind skips ahead to the oldest still-readable slot (the ring's lap rule).
// Returns false when nothing new is readable; per-slot seqlock retries handle
// a producer mid-write. Callers pump until false on each doorbell wake.
bool readNextOutputPacket(const uint8_t* section, uint32_t& lastSeq, OutputPacket& out);

} // namespace hidmaestro
} // namespace satellite
