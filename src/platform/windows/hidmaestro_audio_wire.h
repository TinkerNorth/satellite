// SPDX-License-Identifier: LGPL-3.0-or-later

// Controller-audio shared-memory rings between the elevated HIDMaestro helper
// and satellite. The JSON pipe brokers device lifecycle and is not a PCM
// transport (one 20 ms window is ~2 KB and arrives 50 times a second per
// direction), so an audio persona gets two more sections alongside the
// driver's input/output pair:
//
//   speaker ring — helper writes, satellite reads. The PCM a game wrote to the
//                  pad's speaker/headset endpoint, already reduced to the
//                  channels that cross the wire.
//   mic ring     — satellite writes, helper reads. The PCM the client's
//                  microphone produced, on its way to the pad's mic endpoint.
//
// Each ring is a doorbell event plus a 32-slot ring with a per-slot seqlock,
// exactly the shape the driver's output ring already uses (hidmaestro_wire.h),
// so the drain loop, the lap rule and the torn-slot retry are the same code
// shape a reader here already knows. Unlike the driver's ring, we own BOTH
// ends: the producer marks a slot in progress by zeroing its SeqNo before
// touching the payload, which makes a mid-write slot explicitly unreadable
// instead of merely stale.
//
// This is a private contract between two processes on one machine, so fields
// are host-endian (plain memcpy), unlike the UDP wire in core/types.h. It
// carries no version field either, which means the section SIZES are the
// layout check: satellite maps exactly AUDIO_SECTION_SIZE and a helper built
// against a different layout fails the map rather than misreading PCM.
//
// Sample rates and channel counts are NOT in the slot: they are properties of
// the persona, constant for the life of a plug, and travel once in the plug
// response. The DualSense composite runs 48 kHz both ways; the DualShock 4 v2
// composite runs 32 kHz out / 16 kHz in, matching the hardware it impersonates,
// and the conversion to the pinned wire rate happens on the satellite side
// (core/audio/audio_resampler.h) where it is unit-tested.
//
// Deliberately free of <windows.h>, like hidmaestro_wire.h: the packing is pure
// byte/atomic work, so it links into the portable test target and is verified
// on every CI platform. Section creation, handle duplication and the doorbells
// are the helper's and the adapter's job.
#pragma once

#include <cstddef>
#include <cstdint>

namespace satellite {
namespace hidmaestro {

inline constexpr size_t AUDIO_RING_SLOTS = 32;
inline constexpr size_t AUDIO_RING_HEADER_SIZE = 8; // Head u32 @0, reserved u32 @4

inline constexpr size_t AUDIO_SLOT_SEQNO_OFFSET = 0;    // u32 publish counter / seqlock
inline constexpr size_t AUDIO_SLOT_SERIAL_OFFSET = 4;   // u32 wire serial the PCM belongs to
inline constexpr size_t AUDIO_SLOT_SEQ_OFFSET = 8;      // u16 per-stream sequence, wraps
inline constexpr size_t AUDIO_SLOT_SAMPLES_OFFSET = 10; // u16 interleaved int16 sample count
inline constexpr size_t AUDIO_SLOT_DATA_OFFSET = 12;
inline constexpr size_t AUDIO_SLOT_DATA_CAPACITY = 4096; // bytes
inline constexpr size_t AUDIO_SLOT_SAMPLE_CAPACITY = AUDIO_SLOT_DATA_CAPACITY / sizeof(int16_t);
inline constexpr size_t AUDIO_SLOT_SIZE = AUDIO_SLOT_DATA_OFFSET + AUDIO_SLOT_DATA_CAPACITY; // 4108
inline constexpr size_t AUDIO_SECTION_SIZE =
    AUDIO_RING_HEADER_SIZE + AUDIO_RING_SLOTS * AUDIO_SLOT_SIZE; // 131464

// 4 KB of slot is 1024 stereo frames, ~21 ms at 48 kHz: an order of magnitude
// more than either producer batches, so a batch never has to be split across
// slots and the reader never has to reassemble one.
static_assert(AUDIO_SLOT_SAMPLE_CAPACITY == 2048, "slot holds 2048 interleaved int16 samples");
static_assert(AUDIO_SLOT_SIZE == 4108, "slot layout is the helper contract");
static_assert(AUDIO_SECTION_SIZE == 131464, "section SIZE is the layout check");
// Every field must fit ahead of the payload, and the payload inside the slot.
static_assert(AUDIO_SLOT_SEQ_OFFSET + sizeof(uint16_t) <= AUDIO_SLOT_SAMPLES_OFFSET, "seq fits");
static_assert(AUDIO_SLOT_SAMPLES_OFFSET + sizeof(uint16_t) <= AUDIO_SLOT_DATA_OFFSET,
              "sample count fits");
static_assert(AUDIO_SLOT_SAMPLE_CAPACITY <= UINT16_MAX, "sample count is representable");

// One batch of interleaved int16 PCM, as read out of a ring slot.
struct AudioPacket {
    uint32_t serial = 0;
    uint16_t seq = 0;
    uint16_t sampleCount = 0; // interleaved samples in `data` (frames * channels)
    int16_t data[AUDIO_SLOT_SAMPLE_CAPACITY] = {};
};

// Current publish counter (0 = nothing ever written). Readers snapshot this at
// attach time so a stale ring from a prior plug is skipped rather than replayed.
uint32_t audioRingHead(const uint8_t* section);

// Publish one batch. Single producer per ring (helper for speaker, satellite
// for mic), so reserving the sequence is a plain increment rather than an
// interlocked one. Returns false without writing when the batch does not fit
// AUDIO_SLOT_SAMPLE_CAPACITY: silently truncating audio would be worse than
// dropping a window the caller can log.
bool writeAudioSlot(uint8_t* section, uint32_t serial, uint16_t seq, const int16_t* pcm,
                    uint16_t sampleCount);

// Drain step: copies the batch at lastSeq+1 when one is published and advances
// lastSeq. A reader more than AUDIO_RING_SLOTS behind skips ahead to the oldest
// still-readable slot (the ring's lap rule) — for audio, dropping the backlog
// is right: a listener wants the newest samples, not a late replay of old ones.
// Returns false when nothing new is readable, including while the producer is
// mid-write; callers pump until false on each doorbell wake.
bool readNextAudioSlot(const uint8_t* section, uint32_t& lastSeq, AudioPacket& out);

} // namespace hidmaestro
} // namespace satellite
