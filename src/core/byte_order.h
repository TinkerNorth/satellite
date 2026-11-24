// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>

#include "core/bits.h"

namespace satellite {

// The wire's integers, read and written by explicit shifts so the code is the
// same on either host byte order. Big-endian is the network order the packet
// headers, the audio sequence and DNS use; little-endian is what the pad
// reports carry, since they mirror the HID layout. Every caller guards the
// length first; these read exactly the bytes their width names.

inline uint16_t readBE16(const uint8_t* p) { return bits::loadBe16(p); }

inline uint32_t readBE32(const uint8_t* p) { return bits::loadBe32(p); }

inline void writeBE16(uint8_t* p, uint16_t v) { bits::storeBe16(p, v); }

inline void writeBE32(uint8_t* p, uint32_t v) { bits::storeBe32(p, v); }

inline uint16_t readLE16(const uint8_t* p) { return bits::loadLe16(p); }

// The pad reports carry signed axes: the two's-complement reading of the word.
inline int16_t readLE16s(const uint8_t* p) { return bits::asInt16(bits::loadLe16(p)); }

inline uint32_t readLE32(const uint8_t* p) { return bits::loadLe32(p); }

inline void writeLE16(uint8_t* p, uint16_t v) { bits::storeLe16(p, v); }

inline void writeLE32(uint8_t* p, uint32_t v) { bits::storeLe32(p, v); }

} // namespace satellite
