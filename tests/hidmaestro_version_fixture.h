// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "../src/core/byte_order.h"
#include "../src/core/hidmaestro_version.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace satellite {
namespace hidmaestro {
namespace fixture {

using VersionImage = std::array<uint8_t, VERSION_SECTION_SIZE>;

// Global\HIDMaestroVersion0 for xbox-360-wired under the pinned v2.1.0 SDK on
// its v2.1.0 driver, byte for byte: the default identity of index 0, token
// HM_0000, whose tag hm_contract.h:144-146 gives.
inline constexpr VersionImage PINNED_XBOX_IMAGE = {
    0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x1F, 0x46, 0x8D, 0x6B, 0x91, 0x48, 0xD0, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
inline constexpr uint64_t HM_0000_TAG = 0x1FD048916B8D461FULL;

inline void putI32(VersionImage& image, size_t offset, int32_t value) {
    writeLE32(image.data() + offset, static_cast<uint32_t>(value));
}

inline void putU16(VersionImage& image, size_t offset, uint16_t value) {
    writeLE16(image.data() + offset, value);
}

inline void putU64(VersionImage& image, size_t offset, uint64_t value) {
    writeLE32(image.data() + offset, static_cast<uint32_t>(value));
    writeLE32(image.data() + offset + 4, static_cast<uint32_t>(value >> 32));
}

inline void putVersion(VersionImage& image, size_t offset, uint16_t major, uint16_t minor,
                       uint16_t patch) {
    putU16(image, offset, major);
    putU16(image, offset + 2, minor);
    putU16(image, offset + 4, patch);
    putU16(image, offset + 6, 0);
}

// What the SDK leaves of a section the driver or the companion never wrote:
// zeroes from the create.
inline void eraseCompanionHalf(VersionImage& image) {
    putI32(image, VERSION_COMPANION_PROTOCOL_OFFSET, 0);
    putI32(image, VERSION_COMPANION_PROTOCOL_MIN_OFFSET, 0);
}

inline void eraseDriverHalf(VersionImage& image) {
    putI32(image, VERSION_DRIVER_PROTOCOL_OFFSET, 0);
    putVersion(image, VERSION_DRIVER_VERSION_OFFSET, 0, 0, 0);
    putI32(image, VERSION_DRIVER_CAPABILITIES_OFFSET, 0);
    putI32(image, VERSION_DRIVER_PROTOCOL_MIN_OFFSET, 0);
}

// The section the pinned SDK writes for one of Satellite's controllers once its
// driver has started the device: its own tag, and the companion's pair only on
// an Xbox profile.
inline VersionImage pinnedDriverImage(uint64_t tag, bool companion) {
    VersionImage image = PINNED_XBOX_IMAGE;
    putU64(image, VERSION_IDENTITY_TAG_OFFSET, tag);
    if (!companion) eraseCompanionHalf(image);
    return image;
}

// A composite persona's section: the SDK's USB/IP device serves it, so no
// driver writes a half, and the SDK writes no tag (DeviceOrchestrator.cs:1474,
// the only tag write, is on the UMDF2 path).
inline VersionImage pinnedUsbipImage() {
    VersionImage image = PINNED_XBOX_IMAGE;
    eraseDriverHalf(image);
    eraseCompanionHalf(image);
    putU64(image, VERSION_IDENTITY_TAG_OFFSET, 0);
    return image;
}

} // namespace fixture
} // namespace hidmaestro
} // namespace satellite
