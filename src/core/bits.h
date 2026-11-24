// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Bitwise arithmetic on unsigned values only, so no shift, mask or OR ever has a signed operand.
// A std::uint8_t or std::uint16_t is promoted to int before any operator touches it, and int is
// signed: shifting a byte with its top bit set into the sign bit, or masking a value after it was
// sign-extended, is how a parser goes wrong. So:
//
//   * bytes and half-words widen through u32() before any bitwise operator;
//   * shift counts and masks are unsigned literals (8U, 0xFFU);
//   * a narrower result is taken explicitly (lowByte, low16, byteOf);
//   * a signed value is produced last, by a named conversion (asInt16, asInt32), which is a
//     two's-complement reinterpretation in C++20.
//
// clang-tidy's bugprone-signed-bitwise holds every first-party file to this.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace satellite::bits {

constexpr std::uint32_t u32(std::uint8_t v) noexcept { return v; }
constexpr std::uint32_t u32(std::uint16_t v) noexcept { return v; }
constexpr std::uint32_t u32(std::uint32_t v) noexcept { return v; }
constexpr std::uint64_t u64(std::uint8_t v) noexcept { return v; }
constexpr std::uint64_t u64(std::uint32_t v) noexcept { return v; }
constexpr std::uint64_t u64(std::uint64_t v) noexcept { return v; }

constexpr std::uint8_t lowByte(std::uint32_t v) noexcept {
    return static_cast<std::uint8_t>(v & 0xFFU);
}
constexpr std::uint8_t lowByte(std::uint64_t v) noexcept {
    return static_cast<std::uint8_t>(v & 0xFFU);
}
constexpr std::uint16_t low16(std::uint32_t v) noexcept {
    return static_cast<std::uint16_t>(v & 0xFFFFU);
}

constexpr std::uint8_t highNibble(std::uint8_t v) noexcept { return lowByte(u32(v) >> 4U); }
constexpr std::uint8_t lowNibble(std::uint8_t v) noexcept { return lowByte(u32(v) & 0x0FU); }
constexpr std::uint8_t fromNibbles(std::uint8_t high, std::uint8_t low) noexcept {
    return lowByte((u32(high) << 4U) | u32(low));
}

// Whether any bit of `mask` is set in `value`; a byte or half-word converts without promotion.
constexpr bool anySet(std::uint32_t value, std::uint32_t mask) noexcept {
    return (value & mask) != 0U;
}

// Byte `index` of `v`, counting from the least significant.
constexpr std::uint8_t byteOf(std::uint32_t v, unsigned index) noexcept {
    return lowByte(v >> (8U * index));
}
constexpr std::uint8_t byteOf(std::uint64_t v, unsigned index) noexcept {
    return lowByte(v >> (8U * index));
}

constexpr std::uint16_t loadLe16(const std::uint8_t* p) noexcept {
    return low16(u32(p[0]) | (u32(p[1]) << 8U));
}
constexpr std::uint16_t loadBe16(const std::uint8_t* p) noexcept {
    return low16((u32(p[0]) << 8U) | u32(p[1]));
}
constexpr std::uint32_t loadLe32(const std::uint8_t* p) noexcept {
    return u32(p[0]) | (u32(p[1]) << 8U) | (u32(p[2]) << 16U) | (u32(p[3]) << 24U);
}
constexpr std::uint32_t loadBe32(const std::uint8_t* p) noexcept {
    return (u32(p[0]) << 24U) | (u32(p[1]) << 16U) | (u32(p[2]) << 8U) | u32(p[3]);
}

// Two 12-bit fields packed into three bytes, as HID touch points and the Switch sticks carry
// them: the low field is p[0] with the low nibble of p[1] above it, the high field the high
// nibble of p[0] with p[1] above it (each called at the field's first byte).
constexpr std::uint16_t loadPacked12Low(const std::uint8_t* p) noexcept {
    return low16(u32(p[0]) | ((u32(p[1]) & 0x0FU) << 8U));
}
constexpr std::uint16_t loadPacked12High(const std::uint8_t* p) noexcept {
    return low16((u32(p[0]) >> 4U) | (u32(p[1]) << 4U));
}

constexpr void storeLe16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = byteOf(u32(v), 0U);
    p[1] = byteOf(u32(v), 1U);
}
constexpr void storeBe16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = byteOf(u32(v), 1U);
    p[1] = byteOf(u32(v), 0U);
}
constexpr void storeLe32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (unsigned i = 0; i < 4U; ++i) { p[i] = byteOf(v, i); }
}
constexpr void storeBe32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (unsigned i = 0; i < 4U; ++i) { p[i] = byteOf(v, 3U - i); }
}

constexpr std::int16_t asInt16(std::uint16_t v) noexcept { return std::bit_cast<std::int16_t>(v); }
constexpr std::int32_t asInt32(std::uint32_t v) noexcept { return std::bit_cast<std::int32_t>(v); }
constexpr std::uint16_t asUint16(std::int16_t v) noexcept {
    return std::bit_cast<std::uint16_t>(v);
}
constexpr std::uint32_t asUint32(std::int32_t v) noexcept {
    return std::bit_cast<std::uint32_t>(v);
}

// Two 16-bit ids in one int key, `high` above `low`: (high << 16) | (low & 0xFFFF), shifted
// unsigned, so a high id of 0x8000 or more reaches the sign bit by design rather than by a signed
// shift.
constexpr std::int32_t packHigh16Low16(std::int32_t high, std::int32_t low) noexcept {
    return asInt32((asUint32(high) << 16U) | (asUint32(low) & 0xFFFFU));
}

// Two 32-bit ids in one 64-bit key, `high` above `low`.
constexpr std::int64_t packHigh32Low32(std::int32_t high, std::int32_t low) noexcept {
    return static_cast<std::int64_t>((u64(asUint32(high)) << 32U) | u64(asUint32(low)));
}

// 2 to the power `exponent`, for a doubling written as one; exponent < 63.
constexpr std::int64_t pow2(unsigned exponent) noexcept {
    return static_cast<std::int64_t>(std::uint64_t{1} << exponent);
}

} // namespace satellite::bits
