// SPDX-License-Identifier: LGPL-3.0-or-later
// The controller descriptor as it crosses the client API: the caps and
// touchpad-mode name tables both directions read, and the parser's rules.
// Until this file those rules were asserted only through the route tests,
// which build on Linux and macOS alone.
#include "../src/core/descriptor_json.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "test_util.h"

using satellite::CAP_NAMES;
using satellite::capsFromJson;
using satellite::capsJsonObj;
using satellite::Json;
using satellite::jsonDump;
using satellite::parseControllerDescriptors;
using satellite::parseDescriptorObject;
using satellite::saturateToByte;

namespace {

Json parse(const std::string& text) {
    Json j;
    satellite::jsonParse(text, j);
    return j;
}

constexpr size_t CAP_COUNT = sizeof(CAP_NAMES) / sizeof(CAP_NAMES[0]);
constexpr uint16_t ALL_CAPS = CAP_RUMBLE | CAP_MOTION | CAP_ANALOG_TRIGGERS | CAP_LIGHTBAR |
                              CAP_TRIGGER_EFFECTS | CAP_PLAYER_LEDS | CAP_MIC | CAP_SPEAKER |
                              CAP_HAPTIC_AUDIO;

} // namespace

int main() {
    {
        TEST("the caps table names every wire bit exactly once");
        EXPECT_EQ(CAP_COUNT, static_cast<size_t>(9));
        uint16_t seen = 0;
        for (const auto& cap : CAP_NAMES) {
            EXPECT((seen & cap.bit) == 0);
            seen |= cap.bit;
        }
        EXPECT_EQ(seen, ALL_CAPS);
    }
    {
        TEST("every caps word round-trips through its JSON object");
        for (uint16_t caps = 0; caps <= ALL_CAPS; caps++) {
            if ((caps & ~ALL_CAPS) != 0) continue;
            EXPECT_EQ(capsFromJson(parse(jsonDump(capsJsonObj(caps)))), caps);
        }
    }
    {
        TEST("capsJsonObj writes the keys in the wire's order");
        EXPECT_EQ(jsonDump(capsJsonObj(ALL_CAPS)),
                  std::string(R"({"rumble":true,"motion":true,"analogTriggers":true,)"
                              R"("lightbar":true,"triggerEffects":true,"playerLeds":true,)"
                              R"("mic":true,"speaker":true,"hapticAudio":true})"));
    }
    {
        TEST("an unknown caps key is ignored, and a missing one reads as off");
        EXPECT_EQ(capsFromJson(parse(R"({"jetpack":true,"mic":true})")), CAP_MIC);
        EXPECT_EQ(capsFromJson(parse("{}")), 0);
    }
    {
        TEST("touchpad mode names round-trip, and the unknowns fall back each way");
        for (const auto& entry : TOUCHPAD_MODE_NAMES) {
            EXPECT_EQ(touchpadModeFromName(entry.name), entry.mode);
            EXPECT_EQ(std::string(touchpadModeName(entry.mode)), std::string(entry.name));
        }
        EXPECT_EQ(touchpadModeFromName("trackball"), TOUCHPAD_MODE_OFF);
        EXPECT_EQ(touchpadModeFromName(""), TOUCHPAD_MODE_OFF);
        EXPECT_EQ(std::string(touchpadModeName(200)), std::string("ds4"));
    }
    {
        TEST("saturateToByte keeps the byte range and saturates above it");
        EXPECT_EQ(saturateToByte(0), 0);
        EXPECT_EQ(saturateToByte(255), 255);
        EXPECT_EQ(saturateToByte(256), 255);
        EXPECT_EQ(saturateToByte(100000), 255);
    }
    {
        TEST("parseDescriptorObject: type is required, negative is refused, oversized saturates");
        ControllerDescriptor d;
        EXPECT(!parseDescriptorObject(parse(R"({"ctrlIdx":0})"), true, d));
        EXPECT(!parseDescriptorObject(parse(R"({"ctrlIdx":0,"type":-1})"), true, d));
        EXPECT(parseDescriptorObject(parse(R"({"ctrlIdx":0,"type":999})"), true, d));
        EXPECT_EQ(d.type, 255);
    }
    {
        TEST("parseDescriptorObject: ctrlIdx is required only when asked for, negative is "
             "refused, oversized saturates");
        ControllerDescriptor d;
        EXPECT(!parseDescriptorObject(parse(R"({"type":1})"), true, d));
        EXPECT(parseDescriptorObject(parse(R"({"type":1})"), false, d));
        EXPECT(!parseDescriptorObject(parse(R"({"ctrlIdx":-1,"type":1})"), false, d));
        EXPECT(parseDescriptorObject(parse(R"({"ctrlIdx":300,"type":1})"), false, d));
        EXPECT_EQ(d.ctrlIdx, 255);
    }
    {
        TEST("parseDescriptorObject: caps, preferredBackend and touchpadMode land; the defaults "
             "are none, empty and off");
        ControllerDescriptor d;
        EXPECT(parseDescriptorObject(parse(R"({"ctrlIdx":2,"type":3,)"
                                           R"("caps":{"rumble":true,"speaker":true},)"
                                           R"("preferredBackend":"vigem","touchpadMode":"mouse"})"),
                                     true, d));
        EXPECT_EQ(d.ctrlIdx, 2);
        EXPECT_EQ(d.type, 3);
        EXPECT_EQ(d.caps, static_cast<uint16_t>(CAP_RUMBLE | CAP_SPEAKER));
        EXPECT_EQ(d.preferredBackend, std::string("vigem"));
        EXPECT_EQ(d.touchpadMode, TOUCHPAD_MODE_MOUSE);

        ControllerDescriptor bare;
        EXPECT(parseDescriptorObject(parse(R"({"ctrlIdx":0,"type":0})"), true, bare));
        EXPECT_EQ(bare.caps, 0);
        EXPECT(bare.preferredBackend.empty());
        EXPECT_EQ(bare.touchpadMode, TOUCHPAD_MODE_OFF);
    }
    {
        TEST("parseControllerDescriptors: absent is fine and empty, non-objects are skipped, one "
             "bad entry fails the lot");
        std::vector<ControllerDescriptor> out;
        EXPECT(parseControllerDescriptors(parse("{}"), out));
        EXPECT(out.empty());
        EXPECT(parseControllerDescriptors(parse(R"({"controllers":"nope"})"), out));
        EXPECT(out.empty());
        EXPECT(parseControllerDescriptors(
            parse(R"({"controllers":[7,{"ctrlIdx":1,"type":2},null]})"), out));
        EXPECT_EQ(out.size(), static_cast<size_t>(1));
        EXPECT_EQ(out[0].ctrlIdx, 1);

        std::vector<ControllerDescriptor> bad;
        EXPECT(!parseControllerDescriptors(
            parse(R"({"controllers":[{"ctrlIdx":1,"type":2},{"type":3}]})"), bad));
    }

    std::cout << "test_descriptor_json: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
