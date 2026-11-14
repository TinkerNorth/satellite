// SPDX-License-Identifier: LGPL-3.0-or-later
#include "test_util.h"

#include "../src/core/hidmaestro_version.h"
#include "../src/platform/windows/driver_pins.h"
#include "hidmaestro_version_fixture.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>

namespace satellite {
namespace hidmaestro {

std::ostream& operator<<(std::ostream& os, VersionVerdict verdict) {
    return os << versionVerdictReason(verdict);
}

} // namespace hidmaestro
} // namespace satellite

using namespace satellite::hidmaestro;
using namespace satellite::hidmaestro::fixture;

namespace {

constexpr uint64_t OTHER_TAG = 0x3B86F3634086D831ULL;
constexpr size_t WORD_OFFSETS[] = {0, 4, 8, 20, 32, 36, 40, 44, 56, 60};
constexpr size_t VERSION_OFFSETS[] = {12, 24};
constexpr size_t TAG_OFFSET = 48;

VersionPins shippedPins() {
    return {parseReleaseVersion(SATELLITE_HIDMAESTRO_SDK_VERSION),
            parseReleaseVersion(SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION)};
}

VersionExpectation xboxPad() { return {shippedPins(), ProfileBackend::Umdf2, true, HM_0000_TAG}; }

VersionExpectation sonyPad() { return {shippedPins(), ProfileBackend::Umdf2, false, HM_0000_TAG}; }

VersionExpectation compositePad() {
    return {shippedPins(), ProfileBackend::Usbip, false, HM_0000_TAG};
}

VersionVerdict judge(const VersionImage& image, const VersionExpectation& expectation) {
    return judgeVersionSection(parseVersionSection(image.data()), expectation);
}

bool sameVersion(const SectionVersion& v, uint16_t major, uint16_t minor, uint16_t patch,
                 uint16_t fourth) {
    return v.major == major && v.minor == minor && v.patch == patch && v.fourth == fourth;
}

VersionImage sonyImage() { return pinnedDriverImage(HM_0000_TAG, false); }

} // namespace

static void test_the_pinned_image_decodes_field_for_field() {
    TEST("layout: the pinned SDK's section on its driver (SPEC 7.1) decodes field for field");
    const VersionSection s = parseVersionSection(PINNED_XBOX_IMAGE.data());
    EXPECT_EQ(s.layoutVersion, 1);
    EXPECT_EQ(s.sdkProtocolMin, 4);
    EXPECT_EQ(s.sdkProtocolMax, 4);
    EXPECT(sameVersion(s.sdkVersion, 2, 1, 0, 0));
    EXPECT_EQ(s.driverProtocol, 5);
    EXPECT(sameVersion(s.driverVersion, 2, 1, 0, 0));
    EXPECT_EQ(s.driverCapabilities, 3);
    EXPECT_EQ(s.driverProtocolMin, 4);
    EXPECT_EQ(s.companionProtocol, 4);
    EXPECT_EQ(s.companionProtocolMin, 4);
    EXPECT_EQ(s.identityTag, HM_0000_TAG);
    EXPECT_EQ(s.driverConfigError, 0);
    EXPECT_EQ(s.companionConfigError, 0);
}

static void test_each_field_comes_from_its_upstream_offset() {
    TEST("layout: each field decodes from the offset hm_contract.h gives it, little-endian");
    VersionImage image{};
    const uint8_t marked[] = {0x00, 0x02, 0x03, 0x84};
    for (const size_t offset : WORD_OFFSETS) {
        std::memcpy(image.data() + offset, marked, sizeof(marked));
        image[offset] = static_cast<uint8_t>(offset);
    }
    for (const size_t offset : VERSION_OFFSETS) {
        for (size_t i = 0; i < 8; i++) image[offset + i] = static_cast<uint8_t>(offset + i);
    }
    for (size_t i = 0; i < 8; i++) image[TAG_OFFSET + i] = static_cast<uint8_t>(0xA0 + i);

    const VersionSection s = parseVersionSection(image.data());
    EXPECT_EQ(static_cast<uint32_t>(s.layoutVersion), 0x84030200u);
    EXPECT_EQ(static_cast<uint32_t>(s.sdkProtocolMin), 0x84030204u);
    EXPECT_EQ(static_cast<uint32_t>(s.sdkProtocolMax), 0x84030208u);
    EXPECT(sameVersion(s.sdkVersion, 0x0D0C, 0x0F0E, 0x1110, 0x1312));
    EXPECT_EQ(static_cast<uint32_t>(s.driverProtocol), 0x84030214u);
    EXPECT(sameVersion(s.driverVersion, 0x1918, 0x1B1A, 0x1D1C, 0x1F1E));
    EXPECT_EQ(static_cast<uint32_t>(s.driverCapabilities), 0x84030220u);
    EXPECT_EQ(static_cast<uint32_t>(s.driverProtocolMin), 0x84030224u);
    EXPECT_EQ(static_cast<uint32_t>(s.companionProtocol), 0x84030228u);
    EXPECT_EQ(static_cast<uint32_t>(s.companionProtocolMin), 0x8403022Cu);
    EXPECT_EQ(s.identityTag, 0xA7A6A5A4A3A2A1A0ULL);
    EXPECT_EQ(static_cast<uint32_t>(s.driverConfigError), 0x84030238u);
    EXPECT_EQ(static_cast<uint32_t>(s.companionConfigError), 0x8403023Cu);
}

static void test_a_live_view_reads_as_its_bytes() {
    TEST("read: a live view reads as the bytes it holds");
    const VersionSection s = readVersionSection(PINNED_XBOX_IMAGE.data());
    EXPECT_EQ(s.driverProtocol, 5);
    EXPECT_EQ(s.identityTag, HM_0000_TAG);

    TEST("read: no view is no section, and refuses");
    const VersionCheck check = checkVersionSection(nullptr, xboxPad());
    EXPECT_EQ(check.verdict, VersionVerdict::NoSection);
    EXPECT(versionVerdictRefuses(check.verdict));

    TEST("read: a view is checked against the controller's expectation");
    const VersionCheck written = checkVersionSection(PINNED_XBOX_IMAGE.data(), xboxPad());
    EXPECT_EQ(written.verdict, VersionVerdict::Write);
    EXPECT_EQ(written.section.companionProtocol, 4);
}

static void test_the_pins_are_driver_pins_h() {
    TEST("pins: a release version keeps major.minor.patch, the part the section carries");
    EXPECT(sameVersion(parseReleaseVersion("2.1.0"), 2, 1, 0, 0));
    EXPECT(sameVersion(parseReleaseVersion("2.1.0.1716"), 2, 1, 0, 0));
    EXPECT(sameVersion(parseReleaseVersion("10.20.30"), 10, 20, 30, 0));
    EXPECT(sameVersion(parseReleaseVersion("2.x.0"), 2, 0, 0, 0));

    TEST("pins: driver_pins.h's SDK is the SDK half the pinned SDK writes, and its driver INF "
         "the driver half the pinned driver writes");
    const VersionSection s = parseVersionSection(PINNED_XBOX_IMAGE.data());
    EXPECT(shippedPins().sdk == s.sdkVersion);
    EXPECT(shippedPins().driver == s.driverVersion);
}

static void test_the_pinned_driver_is_written() {
    TEST("policy: the pinned SDK on the pinned driver is written, companion and all");
    EXPECT_EQ(judge(PINNED_XBOX_IMAGE, xboxPad()), VersionVerdict::Write);

    TEST("policy: a pad without a companion is written without the companion's pair");
    EXPECT_EQ(judge(sonyImage(), sonyPad()), VersionVerdict::Write);

    TEST("policy: a pad without a companion ignores what the companion's fields hold");
    VersionImage stray = sonyImage();
    putI32(stray, VERSION_COMPANION_CONFIG_ERROR_OFFSET, 1);
    EXPECT_EQ(judge(stray, sonyPad()), VersionVerdict::Write);
}

static void test_another_driver_that_serves_4_is_written_with_a_warning() {
    TEST("policy: a newer driver that still serves 4 is written, flagged as not the pinned one");
    VersionImage newer = sonyImage();
    putVersion(newer, VERSION_DRIVER_VERSION_OFFSET, 2, 2, 0);
    putI32(newer, VERSION_DRIVER_PROTOCOL_OFFSET, 6);
    EXPECT_EQ(judge(newer, sonyPad()), VersionVerdict::WriteOtherDriver);
    EXPECT(!versionVerdictRefuses(VersionVerdict::WriteOtherDriver));

    TEST("policy: a 2.0.x driver on the same protocols is flagged too: only its version differs");
    VersionImage older = sonyImage();
    putVersion(older, VERSION_DRIVER_VERSION_OFFSET, 2, 0, 3);
    EXPECT_EQ(judge(older, sonyPad()), VersionVerdict::WriteOtherDriver);
}

static void test_another_layout_is_refused_before_anything_else() {
    TEST("policy: a layout other than 1 is refused");
    VersionImage layout = PINNED_XBOX_IMAGE;
    putI32(layout, VERSION_LAYOUT_OFFSET, 2);
    EXPECT_EQ(judge(layout, xboxPad()), VersionVerdict::OtherLayout);

    TEST("policy: the layout is judged first, since no other field means anything without it");
    putU64(layout, VERSION_IDENTITY_TAG_OFFSET, OTHER_TAG);
    putI32(layout, VERSION_DRIVER_CONFIG_ERROR_OFFSET, 1);
    EXPECT_EQ(judge(layout, xboxPad()), VersionVerdict::OtherLayout);
}

static void test_another_controllers_section_is_refused() {
    TEST("policy: a section whose tag is another identity's is refused");
    VersionImage other = PINNED_XBOX_IMAGE;
    putU64(other, VERSION_IDENTITY_TAG_OFFSET, OTHER_TAG);
    EXPECT_EQ(judge(other, xboxPad()), VersionVerdict::OtherController);

    TEST("policy: a tag the SDK has not written yet is not this controller's either");
    putU64(other, VERSION_IDENTITY_TAG_OFFSET, 0);
    EXPECT_EQ(judge(other, xboxPad()), VersionVerdict::OtherController);

    TEST("policy: the tag is judged before the SDK half: another program's SDK is its own");
    VersionImage foreign = PINNED_XBOX_IMAGE;
    putU64(foreign, VERSION_IDENTITY_TAG_OFFSET, OTHER_TAG);
    putVersion(foreign, VERSION_SDK_VERSION_OFFSET, 2, 0, 3);
    EXPECT_EQ(judge(foreign, xboxPad()), VersionVerdict::OtherController);
}

static void test_another_sdk_half_is_refused() {
    TEST("policy: an SDK half that runs on protocol 5 only is refused");
    VersionImage five = PINNED_XBOX_IMAGE;
    putI32(five, VERSION_SDK_PROTOCOL_MIN_OFFSET, 5);
    putI32(five, VERSION_SDK_PROTOCOL_MAX_OFFSET, 5);
    EXPECT_EQ(judge(five, xboxPad()), VersionVerdict::OtherSdk);

    TEST("policy: an SDK half that speaks 5 is not the pinned SDK's, which speaks 4 alone");
    VersionImage wider = PINNED_XBOX_IMAGE;
    putI32(wider, VERSION_SDK_PROTOCOL_MAX_OFFSET, 5);
    EXPECT_EQ(judge(wider, xboxPad()), VersionVerdict::OtherSdk);

    TEST("policy: an SDK half of another release is refused");
    VersionImage older = PINNED_XBOX_IMAGE;
    putVersion(older, VERSION_SDK_VERSION_OFFSET, 2, 0, 3);
    EXPECT_EQ(judge(older, xboxPad()), VersionVerdict::OtherSdk);

    TEST("policy: an SDK half whose fourth version field is set is not the pinned SDK's");
    VersionImage fourth = PINNED_XBOX_IMAGE;
    putU16(fourth, VERSION_SDK_VERSION_OFFSET + 6, 1);
    EXPECT_EQ(judge(fourth, xboxPad()), VersionVerdict::OtherSdk);
}

static void test_a_driver_that_failed_a_start_is_refused_before_its_range() {
    TEST("policy: a driver error word is refused, whatever the range says");
    VersionImage error = PINNED_XBOX_IMAGE;
    putI32(error, VERSION_DRIVER_CONFIG_ERROR_OFFSET, 1);
    EXPECT_EQ(judge(error, xboxPad()), VersionVerdict::DriverError);

    TEST("policy: the error word is judged before the range, the order the SDK reads them in");
    putI32(error, VERSION_DRIVER_PROTOCOL_MIN_OFFSET, 5);
    putI32(error, VERSION_DRIVER_PROTOCOL_OFFSET, 6);
    EXPECT_EQ(judge(error, xboxPad()), VersionVerdict::DriverError);

    TEST("policy: an error word left beside a zeroed protocol still refuses");
    VersionImage forgotten = PINNED_XBOX_IMAGE;
    putI32(forgotten, VERSION_DRIVER_CONFIG_ERROR_OFFSET, 3);
    putI32(forgotten, VERSION_DRIVER_PROTOCOL_OFFSET, 0);
    EXPECT_EQ(judge(forgotten, xboxPad()), VersionVerdict::DriverError);
}

static void test_an_unwritten_driver_half_waits_then_refuses() {
    TEST("policy: a driver half not written yet waits");
    VersionImage unwritten = sonyImage();
    eraseDriverHalf(unwritten);
    EXPECT_EQ(judge(unwritten, sonyPad()), VersionVerdict::Pending);
    EXPECT(!versionVerdictRefuses(VersionVerdict::Pending));

    TEST("policy: a zero protocol waits even beside a stale range and version");
    VersionImage stale = sonyImage();
    putI32(stale, VERSION_DRIVER_PROTOCOL_OFFSET, 0);
    EXPECT_EQ(judge(stale, sonyPad()), VersionVerdict::Pending);

    TEST("policy: still unwritten when the wait runs out is refused");
    EXPECT_EQ(settleVersionVerdict(VersionVerdict::Pending, true), VersionVerdict::Unwritten);
    EXPECT(versionVerdictRefuses(VersionVerdict::Unwritten));

    TEST("policy: inside the wait it keeps waiting, and the wait changes no other verdict");
    EXPECT_EQ(settleVersionVerdict(VersionVerdict::Pending, false), VersionVerdict::Pending);
    EXPECT_EQ(settleVersionVerdict(VersionVerdict::Write, true), VersionVerdict::Write);
    EXPECT_EQ(settleVersionVerdict(VersionVerdict::DriverProtocol, true),
              VersionVerdict::DriverProtocol);
}

static void test_a_driver_that_does_not_serve_4_is_refused() {
    TEST("policy: a driver that dropped protocol 4 is refused");
    VersionImage dropped = sonyImage();
    putI32(dropped, VERSION_DRIVER_PROTOCOL_MIN_OFFSET, 5);
    putI32(dropped, VERSION_DRIVER_PROTOCOL_OFFSET, 6);
    EXPECT_EQ(judge(dropped, sonyPad()), VersionVerdict::DriverProtocol);

    TEST("policy: a driver older than protocol 4 is refused");
    VersionImage older = sonyImage();
    putI32(older, VERSION_DRIVER_PROTOCOL_MIN_OFFSET, 1);
    putI32(older, VERSION_DRIVER_PROTOCOL_OFFSET, 3);
    EXPECT_EQ(judge(older, sonyPad()), VersionVerdict::DriverProtocol);

    TEST("policy: the range's both ends count: 4 to 4 serves, 5 to 5 does not");
    VersionImage exact = sonyImage();
    putI32(exact, VERSION_DRIVER_PROTOCOL_OFFSET, 4);
    EXPECT_EQ(judge(exact, sonyPad()), VersionVerdict::Write);
    putI32(exact, VERSION_DRIVER_PROTOCOL_MIN_OFFSET, 5);
    putI32(exact, VERSION_DRIVER_PROTOCOL_OFFSET, 5);
    EXPECT_EQ(judge(exact, sonyPad()), VersionVerdict::DriverProtocol);
}

static void test_the_xusb_companion_is_judged_on_a_companion_profile() {
    TEST("policy: a companion error word is refused");
    VersionImage error = PINNED_XBOX_IMAGE;
    putI32(error, VERSION_COMPANION_CONFIG_ERROR_OFFSET, 1);
    EXPECT_EQ(judge(error, xboxPad()), VersionVerdict::CompanionError);

    TEST("policy: the companion's error word is judged before its range");
    putI32(error, VERSION_COMPANION_PROTOCOL_MIN_OFFSET, 5);
    putI32(error, VERSION_COMPANION_PROTOCOL_OFFSET, 5);
    EXPECT_EQ(judge(error, xboxPad()), VersionVerdict::CompanionError);

    TEST("policy: a companion that does not serve 4 is refused");
    VersionImage dropped = PINNED_XBOX_IMAGE;
    putI32(dropped, VERSION_COMPANION_PROTOCOL_MIN_OFFSET, 5);
    putI32(dropped, VERSION_COMPANION_PROTOCOL_OFFSET, 5);
    EXPECT_EQ(judge(dropped, xboxPad()), VersionVerdict::CompanionProtocol);

    TEST("policy: an unwritten companion pair is written, not held: the companion writes it only "
         "at its add, and a create that finds it running restarts it without waiting");
    VersionImage unwritten = PINNED_XBOX_IMAGE;
    eraseCompanionHalf(unwritten);
    EXPECT_EQ(judge(unwritten, xboxPad()), VersionVerdict::Write);

    TEST("policy: an unwritten companion pair still answers to its error word");
    putI32(unwritten, VERSION_COMPANION_CONFIG_ERROR_OFFSET, 1);
    EXPECT_EQ(judge(unwritten, xboxPad()), VersionVerdict::CompanionError);

    TEST("policy: a driver refusal outranks the companion's");
    VersionImage both = PINNED_XBOX_IMAGE;
    putI32(both, VERSION_DRIVER_CONFIG_ERROR_OFFSET, 1);
    putI32(both, VERSION_COMPANION_CONFIG_ERROR_OFFSET, 1);
    EXPECT_EQ(judge(both, xboxPad()), VersionVerdict::DriverError);

    TEST("policy: a companion refusal outranks a driver half still to come");
    VersionImage waiting = PINNED_XBOX_IMAGE;
    eraseDriverHalf(waiting);
    putI32(waiting, VERSION_COMPANION_CONFIG_ERROR_OFFSET, 1);
    EXPECT_EQ(judge(waiting, xboxPad()), VersionVerdict::CompanionError);

    TEST("policy: an unpinned driver beside a good companion is still written, flagged");
    VersionImage newer = PINNED_XBOX_IMAGE;
    putVersion(newer, VERSION_DRIVER_VERSION_OFFSET, 3, 0, 0);
    EXPECT_EQ(judge(newer, xboxPad()), VersionVerdict::WriteOtherDriver);
}

static void test_a_composite_persona_has_no_driver_half() {
    TEST("policy: a composite persona's section, SDK half alone, is written");
    EXPECT_EQ(judge(pinnedUsbipImage(), compositePad()), VersionVerdict::Write);

    TEST("policy: a composite persona's section carries no tag, whatever its key's tag");
    VersionImage tagged = pinnedUsbipImage();
    putU64(tagged, VERSION_IDENTITY_TAG_OFFSET, HM_0000_TAG);
    EXPECT_EQ(judge(tagged, compositePad()), VersionVerdict::OtherController);

    TEST("policy: a composite persona's SDK half and layout are judged as any other");
    VersionImage older = pinnedUsbipImage();
    putVersion(older, VERSION_SDK_VERSION_OFFSET, 2, 0, 3);
    EXPECT_EQ(judge(older, compositePad()), VersionVerdict::OtherSdk);
    VersionImage layout = pinnedUsbipImage();
    putI32(layout, VERSION_LAYOUT_OFFSET, 2);
    EXPECT_EQ(judge(layout, compositePad()), VersionVerdict::OtherLayout);

    TEST("policy: the same zeroed driver half on a driver-served pad waits instead");
    const VersionExpectation untagged{shippedPins(), ProfileBackend::Umdf2, false, 0};
    EXPECT_EQ(judge(pinnedUsbipImage(), untagged), VersionVerdict::Pending);
}

static void test_a_section_opens_holds_or_refuses_its_pad() {
    TEST("gate: a section that passes opens the pad, one whose driver half is still to come "
         "holds it, and one that fails refuses it");
    VersionImage unwritten = sonyImage();
    eraseDriverHalf(unwritten);
    VersionImage foreign = sonyImage();
    putU64(foreign, VERSION_IDENTITY_TAG_OFFSET, OTHER_TAG);
    EXPECT(writeGateFor(judge(sonyImage(), sonyPad())) == WriteGate::Open);
    EXPECT(writeGateFor(judge(unwritten, sonyPad())) == WriteGate::Held);
    EXPECT(writeGateFor(judge(foreign, sonyPad())) == WriteGate::Refused);
    EXPECT(versionVerdictRefuses(judge(foreign, sonyPad())));
    EXPECT(!versionVerdictRefuses(judge(unwritten, sonyPad())));

    TEST("gate: the wait running out refuses a held pad and nothing else");
    EXPECT(writeGateFor(settleVersionVerdict(judge(unwritten, sonyPad()), true)) ==
           WriteGate::Refused);
    EXPECT(writeGateFor(settleVersionVerdict(judge(sonyImage(), sonyPad()), true)) ==
           WriteGate::Open);
    EXPECT(std::string(versionVerdictReason(VersionVerdict::Unwritten)).find("within the wait") !=
           std::string::npos);
}

static void test_the_hold_lasts_as_long_as_the_sdk_would_wait() {
    TEST("deadline: the hold stretches by the factor the SDK stretches its own waits by, across "
         "the range the SDK takes");
    const auto base = std::chrono::milliseconds(4000);
    EXPECT(scaleHalfDeadline(base, 1.0) == base);
    EXPECT(scaleHalfDeadline(base, 2.5) == std::chrono::milliseconds(10000));
    EXPECT(scaleHalfDeadline(base, 0.1) == std::chrono::milliseconds(400));
    EXPECT(scaleHalfDeadline(base, 100.0) == std::chrono::milliseconds(400000));

    TEST("deadline: a factor the SDK would not have used leaves the deadline unscaled");
    EXPECT(scaleHalfDeadline(base, 0.0) == base);
    EXPECT(scaleHalfDeadline(base, -3.0) == base);
    EXPECT(scaleHalfDeadline(base, 101.0) == base);
    EXPECT(scaleHalfDeadline(base, std::nan("")) == base);
}

static void test_the_log_names_what_the_section_holds() {
    TEST("log: a section is described by what each writer put in it");
    EXPECT_EQ(describeVersionSection(parseVersionSection(PINNED_XBOX_IMAGE.data())),
              std::string("layout 1, SDK 2.1.0 protocols 4 to 4, driver 2.1.0 protocols 4 to 5 "
                          "error 0, companion protocols 4 to 4 error 0, tag 1fd048916b8d461f"));

    TEST("log: a version's fourth field shows only when it is set");
    EXPECT_EQ(describeSectionVersion({2, 1, 0, 0}), std::string("2.1.0"));
    EXPECT_EQ(describeSectionVersion({2, 1, 0, 7}), std::string("2.1.0.7"));
}

int main() {
    test_the_pinned_image_decodes_field_for_field();
    test_each_field_comes_from_its_upstream_offset();
    test_a_live_view_reads_as_its_bytes();
    test_the_pins_are_driver_pins_h();
    test_the_pinned_driver_is_written();
    test_another_driver_that_serves_4_is_written_with_a_warning();
    test_another_layout_is_refused_before_anything_else();
    test_another_controllers_section_is_refused();
    test_another_sdk_half_is_refused();
    test_a_driver_that_failed_a_start_is_refused_before_its_range();
    test_an_unwritten_driver_half_waits_then_refuses();
    test_a_driver_that_does_not_serve_4_is_refused();
    test_the_xusb_companion_is_judged_on_a_companion_profile();
    test_a_composite_persona_has_no_driver_half();
    test_a_section_opens_holds_or_refuses_its_pad();
    test_the_hold_lasts_as_long_as_the_sdk_would_wait();
    test_the_log_names_what_the_section_holds();

    std::cout << "hidmaestro_version: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
