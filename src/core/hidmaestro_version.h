// SPDX-License-Identifier: LGPL-3.0-or-later

// Global\HIDMaestroVersion<N>, HIDMaestro v2.1.0's version contract
// (HIDMAESTRO_VERSION_INFO, driver/hm_contract.h:47-65; SharedMemoryIO.cs
// 194-216): 64 bytes, little-endian, one per transport index. The SDK writes
// its half before the device exists, the HID driver its half at add with
// DriverProtocol last, the XUSB companion its own pair. Nothing is ever
// cleared and nothing signals a change, so a writer reads it again.
#pragma once

#include "core/byte_order.h"
#include "core/hex.h"
#include "core/semver.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace satellite {
namespace hidmaestro {

inline constexpr size_t VERSION_SECTION_SIZE = 64;
inline constexpr size_t VERSION_LAYOUT_OFFSET = 0;                  // SDK
inline constexpr size_t VERSION_SDK_PROTOCOL_MIN_OFFSET = 4;        // SDK
inline constexpr size_t VERSION_SDK_PROTOCOL_MAX_OFFSET = 8;        // SDK
inline constexpr size_t VERSION_SDK_VERSION_OFFSET = 12;            // SDK
inline constexpr size_t VERSION_DRIVER_PROTOCOL_OFFSET = 20;        // HID driver, stored last
inline constexpr size_t VERSION_DRIVER_VERSION_OFFSET = 24;         // HID driver
inline constexpr size_t VERSION_DRIVER_CAPABILITIES_OFFSET = 32;    // HID driver
inline constexpr size_t VERSION_DRIVER_PROTOCOL_MIN_OFFSET = 36;    // HID driver
inline constexpr size_t VERSION_COMPANION_PROTOCOL_OFFSET = 40;     // XUSB companion, stored last
inline constexpr size_t VERSION_COMPANION_PROTOCOL_MIN_OFFSET = 44; // XUSB companion
inline constexpr size_t VERSION_IDENTITY_TAG_OFFSET = 48;           // SDK
inline constexpr size_t VERSION_DRIVER_CONFIG_ERROR_OFFSET = 56;    // HID driver
inline constexpr size_t VERSION_COMPANION_CONFIG_ERROR_OFFSET = 60; // XUSB companion

inline constexpr int32_t VERSION_LAYOUT = 1;
// The pinned SDK speaks protocol 4 and runs on it alone (SDK_PROTOCOL_MIN and
// SDK_PROTOCOL, SharedMemoryIO.cs:216).
inline constexpr int32_t SDK_PROTOCOL = 4;

// The SDK's create returns without the driver half when no HID child appeared
// within its own wait, so such a pad is plugged but held, nothing written to
// it, until the half comes or this deadline passes.
inline constexpr int VERSION_HALF_DEADLINE_MS = 15000;
inline constexpr int VERSION_READ_ATTEMPTS = 4;

// USHORT[4]: major, minor, patch, and a fourth field every v2.1.0 writer
// leaves 0. The driver INF's build number is not in the section.
struct SectionVersion {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;
    uint16_t fourth = 0;
};

inline bool operator==(const SectionVersion& a, const SectionVersion& b) {
    return a.major == b.major && a.minor == b.minor && a.patch == b.patch && a.fourth == b.fourth;
}

inline bool operator!=(const SectionVersion& a, const SectionVersion& b) { return !(a == b); }

// HIDMAESTRO_VERSION_INFO field for field. The C struct is packed, but every
// field is naturally aligned, so this one has its layout without packing.
struct VersionSection {
    int32_t layoutVersion = 0;
    int32_t sdkProtocolMin = 0;
    int32_t sdkProtocolMax = 0;
    SectionVersion sdkVersion;
    int32_t driverProtocol = 0;
    SectionVersion driverVersion;
    int32_t driverCapabilities = 0;
    int32_t driverProtocolMin = 0;
    int32_t companionProtocol = 0;
    int32_t companionProtocolMin = 0;
    uint64_t identityTag = 0;
    int32_t driverConfigError = 0;
    int32_t companionConfigError = 0;
};

static_assert(sizeof(VersionSection) == VERSION_SECTION_SIZE, "hm_contract.h:62");
static_assert(offsetof(VersionSection, layoutVersion) == VERSION_LAYOUT_OFFSET);
static_assert(offsetof(VersionSection, sdkProtocolMin) == VERSION_SDK_PROTOCOL_MIN_OFFSET);
static_assert(offsetof(VersionSection, sdkProtocolMax) == VERSION_SDK_PROTOCOL_MAX_OFFSET);
static_assert(offsetof(VersionSection, sdkVersion) == VERSION_SDK_VERSION_OFFSET);
static_assert(offsetof(VersionSection, driverProtocol) == VERSION_DRIVER_PROTOCOL_OFFSET);
static_assert(offsetof(VersionSection, driverVersion) == VERSION_DRIVER_VERSION_OFFSET);
static_assert(offsetof(VersionSection, driverCapabilities) == VERSION_DRIVER_CAPABILITIES_OFFSET);
static_assert(offsetof(VersionSection, driverProtocolMin) == VERSION_DRIVER_PROTOCOL_MIN_OFFSET,
              "hm_contract.h:63");
static_assert(offsetof(VersionSection, companionProtocol) == VERSION_COMPANION_PROTOCOL_OFFSET);
static_assert(offsetof(VersionSection, companionProtocolMin) ==
              VERSION_COMPANION_PROTOCOL_MIN_OFFSET);
static_assert(offsetof(VersionSection, identityTag) == VERSION_IDENTITY_TAG_OFFSET,
              "hm_contract.h:64");
static_assert(offsetof(VersionSection, driverConfigError) == VERSION_DRIVER_CONFIG_ERROR_OFFSET,
              "hm_contract.h:65");
static_assert(offsetof(VersionSection, companionConfigError) ==
              VERSION_COMPANION_CONFIG_ERROR_OFFSET);

// Who serves the controller: the HIDMaestro UMDF2 driver, or for a composite
// persona the SDK's own USB/IP device, which writes no driver half
// (HMProfile.RequiresUsbipBackend).
enum class ProfileBackend : uint8_t { Umdf2, Usbip };

// The SDK and the driver Satellite ships, as the section carries them.
struct VersionPins {
    SectionVersion sdk;
    SectionVersion driver;
};

// What one controller's section must hold for Satellite to write it.
struct VersionExpectation {
    VersionPins pins;
    ProfileBackend backend = ProfileBackend::Umdf2;
    // An XUSB companion serves the profile (xbox-360-wired).
    bool companion = false;
    // The tag the SDK writes for the controller's identity key, as the helper
    // derives it.
    uint64_t identityTag = 0;
};

enum class VersionVerdict : uint8_t {
    Write,
    WriteOtherDriver,
    Pending,
    NoSection,
    OtherLayout,
    OtherController,
    OtherSdk,
    DriverError,
    DriverProtocol,
    CompanionError,
    CompanionProtocol,
    Unwritten,
};

// What a verdict does to a pad: its input is written while Open, held while
// its driver's half is still to come, and a Refused pad is inert both ways.
enum class WriteGate : uint8_t { Open, Held, Refused };

inline WriteGate writeGateFor(VersionVerdict verdict) {
    switch (verdict) {
    case VersionVerdict::Write:
    case VersionVerdict::WriteOtherDriver:
        return WriteGate::Open;
    case VersionVerdict::Pending:
        return WriteGate::Held;
    case VersionVerdict::NoSection:
    case VersionVerdict::OtherLayout:
    case VersionVerdict::OtherController:
    case VersionVerdict::OtherSdk:
    case VersionVerdict::DriverError:
    case VersionVerdict::DriverProtocol:
    case VersionVerdict::CompanionError:
    case VersionVerdict::CompanionProtocol:
    case VersionVerdict::Unwritten:
        return WriteGate::Refused;
    }
    return WriteGate::Refused;
}

inline bool versionVerdictRefuses(VersionVerdict verdict) {
    return writeGateFor(verdict) == WriteGate::Refused;
}

struct VersionCheck {
    VersionVerdict verdict = VersionVerdict::NoSection;
    VersionSection section;
};

inline SectionVersion readSectionVersion(const uint8_t* p) {
    return {readLE16(p), readLE16(p + 2), readLE16(p + 4), readLE16(p + 6)};
}

inline int32_t readLE32s(const uint8_t* p) { return static_cast<int32_t>(readLE32(p)); }

inline uint64_t readLE64(const uint8_t* p) {
    return static_cast<uint64_t>(readLE32(p)) | (static_cast<uint64_t>(readLE32(p + 4)) << 32);
}

// `bytes` holds VERSION_SECTION_SIZE bytes.
inline VersionSection parseVersionSection(const uint8_t* bytes) {
    VersionSection s;
    s.layoutVersion = readLE32s(bytes + VERSION_LAYOUT_OFFSET);
    s.sdkProtocolMin = readLE32s(bytes + VERSION_SDK_PROTOCOL_MIN_OFFSET);
    s.sdkProtocolMax = readLE32s(bytes + VERSION_SDK_PROTOCOL_MAX_OFFSET);
    s.sdkVersion = readSectionVersion(bytes + VERSION_SDK_VERSION_OFFSET);
    s.driverProtocol = readLE32s(bytes + VERSION_DRIVER_PROTOCOL_OFFSET);
    s.driverVersion = readSectionVersion(bytes + VERSION_DRIVER_VERSION_OFFSET);
    s.driverCapabilities = readLE32s(bytes + VERSION_DRIVER_CAPABILITIES_OFFSET);
    s.driverProtocolMin = readLE32s(bytes + VERSION_DRIVER_PROTOCOL_MIN_OFFSET);
    s.companionProtocol = readLE32s(bytes + VERSION_COMPANION_PROTOCOL_OFFSET);
    s.companionProtocolMin = readLE32s(bytes + VERSION_COMPANION_PROTOCOL_MIN_OFFSET);
    s.identityTag = readLE64(bytes + VERSION_IDENTITY_TAG_OFFSET);
    s.driverConfigError = readLE32s(bytes + VERSION_DRIVER_CONFIG_ERROR_OFFSET);
    s.companionConfigError = readLE32s(bytes + VERSION_COMPANION_CONFIG_ERROR_OFFSET);
    return s;
}

// DriverProtocol is the driver's last store, after a barrier (hm_contract.h:
// 22-24), so a copy taken between two equal reads of it holds the driver half
// that goes with it. A re-announce of the same build writes the same bytes.
inline VersionSection readVersionSection(const uint8_t* view) {
    uint8_t copy[VERSION_SECTION_SIZE] = {};
    for (int attempt = 0; attempt < VERSION_READ_ATTEMPTS; ++attempt) {
        const uint32_t before = readLE32(view + VERSION_DRIVER_PROTOCOL_OFFSET);
        std::atomic_thread_fence(std::memory_order_acquire);
        std::memcpy(copy, view, VERSION_SECTION_SIZE);
        std::atomic_thread_fence(std::memory_order_acquire);
        const bool steady = readLE32(view + VERSION_DRIVER_PROTOCOL_OFFSET) == before;
        if (steady) break;
    }
    return parseVersionSection(copy);
}

inline bool servesSdkProtocol(int32_t oldest, int32_t newest) {
    return oldest <= SDK_PROTOCOL && SDK_PROTOCOL <= newest;
}

inline bool writtenByPinnedSdk(const VersionSection& s, const VersionPins& pins) {
    const bool speaksPinnedProtocol =
        s.sdkProtocolMin == SDK_PROTOCOL && s.sdkProtocolMax == SDK_PROTOCOL;
    return speaksPinnedProtocol && s.sdkVersion == pins.sdk;
}

// The SDK writes a tag only on the UMDF2 path (DeviceOrchestrator.cs:1474).
inline uint64_t expectedIdentityTag(const VersionExpectation& e) {
    switch (e.backend) {
    case ProfileBackend::Umdf2:
        return e.identityTag;
    case ProfileBackend::Usbip:
        return 0;
    }
    return e.identityTag;
}

// The error word before the range, the order the SDK checks them in
// (DeviceOrchestrator.cs:2012-2014): a failed start announces first and names
// its value after, and a forgotten protocol leaves the error word behind.
inline VersionVerdict judgeDriverHalf(const VersionSection& s, const VersionPins& pins) {
    const bool serves = servesSdkProtocol(s.driverProtocolMin, s.driverProtocol);
    if (s.driverConfigError != 0) return VersionVerdict::DriverError;
    if (s.driverProtocol == 0) return VersionVerdict::Pending;
    if (!serves) return VersionVerdict::DriverProtocol;
    if (s.driverVersion != pins.driver) return VersionVerdict::WriteOtherDriver;
    return VersionVerdict::Write;
}

inline VersionVerdict judgeCompanionHalf(const VersionSection& s) {
    const bool serves = servesSdkProtocol(s.companionProtocolMin, s.companionProtocol);
    if (s.companionConfigError != 0) return VersionVerdict::CompanionError;
    if (s.companionProtocol == 0) return VersionVerdict::Pending;
    if (!serves) return VersionVerdict::CompanionProtocol;
    return VersionVerdict::Write;
}

inline VersionVerdict judgeDriverHalves(const VersionSection& s, const VersionExpectation& e) {
    const VersionVerdict driver = judgeDriverHalf(s, e.pins);
    const VersionVerdict companion = e.companion ? judgeCompanionHalf(s) : VersionVerdict::Write;
    const bool waiting = driver == VersionVerdict::Pending || companion == VersionVerdict::Pending;
    if (versionVerdictRefuses(driver)) return driver;
    if (versionVerdictRefuses(companion)) return companion;
    return waiting ? VersionVerdict::Pending : driver;
}

inline VersionVerdict judgeServedHalves(const VersionSection& s, const VersionExpectation& e) {
    switch (e.backend) {
    case ProfileBackend::Umdf2:
        return judgeDriverHalves(s, e);
    case ProfileBackend::Usbip:
        return VersionVerdict::Write;
    }
    return VersionVerdict::Write;
}

// The SDK accepts a newer driver that still serves 4 (DeviceOrchestrator.cs:
// 2014), so a driver other than the pinned one is written, flagged; refusing
// it would stop Satellite's pads whenever another program installs one.
inline VersionVerdict judgeVersionSection(const VersionSection& s, const VersionExpectation& e) {
    if (s.layoutVersion != VERSION_LAYOUT) return VersionVerdict::OtherLayout;
    if (s.identityTag != expectedIdentityTag(e)) return VersionVerdict::OtherController;
    if (!writtenByPinnedSdk(s, e.pins)) return VersionVerdict::OtherSdk;
    return judgeServedHalves(s, e);
}

inline VersionVerdict settleVersionVerdict(VersionVerdict verdict, bool waitedOut) {
    const bool stillPending = verdict == VersionVerdict::Pending && waitedOut;
    return stillPending ? VersionVerdict::Unwritten : verdict;
}

inline VersionCheck checkVersionSection(const uint8_t* view, const VersionExpectation& e) {
    if (view == nullptr) return {};
    const VersionSection section = readVersionSection(view);
    return {judgeVersionSection(section, e), section};
}

// The log's words for a verdict, after "Serial N".
inline const char* versionVerdictReason(VersionVerdict verdict) {
    switch (verdict) {
    case VersionVerdict::Write:
        return "runs on the pinned HIDMaestro driver";
    case VersionVerdict::WriteOtherDriver:
        return "runs on another HIDMaestro driver release that still serves protocol 4";
    case VersionVerdict::Pending:
        return "has a driver or XUSB companion that has not written its half yet";
    case VersionVerdict::NoSection:
        return "came with no HIDMaestro version section";
    case VersionVerdict::OtherLayout:
        return "has a version section of a layout other than 1";
    case VersionVerdict::OtherController:
        return "has a version section whose identity tag is another controller's";
    case VersionVerdict::OtherSdk:
        return "has a version section another HIDMaestro SDK wrote";
    case VersionVerdict::DriverError:
        return "has a driver that failed its start and named the value it could not read";
    case VersionVerdict::DriverProtocol:
        return "has a driver that does not serve protocol 4";
    case VersionVerdict::CompanionError:
        return "has an XUSB companion that failed its start and named the value it could not read";
    case VersionVerdict::CompanionProtocol:
        return "has an XUSB companion that does not serve protocol 4";
    case VersionVerdict::Unwritten:
        return "has a driver or XUSB companion that wrote no protocol within the wait";
    }
    return "";
}

inline std::string describeSectionVersion(const SectionVersion& v) {
    const std::string release =
        std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    return v.fourth == 0 ? release : release + "." + std::to_string(v.fourth);
}

inline std::string describeProtocols(int32_t oldest, int32_t newest) {
    return "protocols " + std::to_string(oldest) + " to " + std::to_string(newest);
}

inline std::string describeVersionSection(const VersionSection& s) {
    const std::string sdk = "SDK " + describeSectionVersion(s.sdkVersion) + " " +
                            describeProtocols(s.sdkProtocolMin, s.sdkProtocolMax);
    const std::string driver = "driver " + describeSectionVersion(s.driverVersion) + " " +
                               describeProtocols(s.driverProtocolMin, s.driverProtocol) +
                               " error " + std::to_string(s.driverConfigError);
    const std::string companion = "companion " +
                                  describeProtocols(s.companionProtocolMin, s.companionProtocol) +
                                  " error " + std::to_string(s.companionConfigError);
    const std::string tag = hexEncodeBE32(static_cast<uint32_t>(s.identityTag >> 32)) +
                            hexEncodeBE32(static_cast<uint32_t>(s.identityTag));
    return "layout " + std::to_string(s.layoutVersion) + ", " + sdk + ", " + driver + ", " +
           companion + ", tag " + tag;
}

// One dotted part as a USHORT; a part that is not a number reads as 0.
inline uint16_t releasePart(const std::vector<std::string>& parts, size_t index) {
    if (index >= parts.size() || !semverIdentIsNumeric(parts[index])) return 0;
    uint32_t value = 0;
    for (const char digit : parts[index]) {
        value = value * 10 + static_cast<uint32_t>(digit - '0');
        if (value > UINT16_MAX) return 0;
    }
    return static_cast<uint16_t>(value);
}

// "2.1.0" or the driver INF's "2.1.0.1716" as the section carries a release:
// major.minor.patch, the fourth field 0.
inline SectionVersion parseReleaseVersion(const std::string& dotted) {
    const std::vector<std::string> parts = semverSplitDots(dotted);
    return {releasePart(parts, 0), releasePart(parts, 1), releasePart(parts, 2), 0};
}

} // namespace hidmaestro
} // namespace satellite
