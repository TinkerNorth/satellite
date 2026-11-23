// SPDX-License-Identifier: LGPL-3.0-or-later

// The dashboard's Test button: one burst of the existing feedback messages to
// a controller's client, each lane only when its descriptor claimed it. The
// plan and the bytes are pure; SessionService::testController sends them.
#pragma once

#include "json.h"
#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace satellite {

enum class FeedbackLane : uint8_t { Rumble, Lightbar, PlayerLeds, TriggerEffects, MicLed };

struct FeedbackLaneInfo {
    FeedbackLane lane;
    const char* name;
    // The descriptor cap that admits the lane's message, the same gate the
    // game's own writes pass. MIC_LED rides CAP_MIC (see MSG_MIC_LED).
    uint16_t cap;
    // State the game set: the test holds the surface for TEST_HOLD_MS, then
    // the game's last value (or off) goes back. Rumble simply runs out.
    bool held;
};

inline constexpr FeedbackLaneInfo FEEDBACK_LANES[] = {
    {FeedbackLane::Rumble, "rumble", CAP_RUMBLE, false},
    {FeedbackLane::Lightbar, "lightbar", CAP_LIGHTBAR, true},
    {FeedbackLane::PlayerLeds, "playerLeds", CAP_PLAYER_LEDS, true},
    {FeedbackLane::TriggerEffects, "triggerEffects", CAP_TRIGGER_EFFECTS, true},
    {FeedbackLane::MicLed, "micLed", CAP_MIC, true},
};

// The bench's time for the lamp and the triggers, long enough to see.
inline constexpr int TEST_HOLD_MS = 2000;

constexpr uint8_t feedbackLaneBit(FeedbackLane lane) {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(lane));
}

// Half of each motor's u16 range, for half a second.
inline constexpr uint16_t TEST_RUMBLE_MAGNITUDE = 0x8000;
inline constexpr uint16_t TEST_RUMBLE_DURATION_MS = 500;
inline constexpr uint8_t TEST_LIGHTBAR_LEVEL = 0xFF;
inline constexpr uint8_t TEST_PLAYER_LEDS_MASK = 0x01;
// DualSense trigger-effect mode 0x01, continuous resistance: param 0 is the
// start position, param 1 the force. Mild: from the top, a quarter force.
inline constexpr uint8_t TEST_TRIGGER_MODE_RESISTANCE = 0x01;
inline constexpr uint8_t TEST_TRIGGER_START = 0x00;
inline constexpr uint8_t TEST_TRIGGER_FORCE = 0x40;
inline constexpr uint8_t TEST_MIC_LED_STATE = MIC_LED_STATE_PULSE;

const char* feedbackLaneName(FeedbackLane lane);
bool parseFeedbackLane(const std::string& name, FeedbackLane& out);

struct FeedbackTestPlan {
    // Both in FEEDBACK_LANES order, each lane at most once.
    std::vector<FeedbackLane> sent;
    std::vector<FeedbackLane> skipped;
};

// The route body's "lanes": an array of lane names. False, with the offending
// value in `bad`, when it is absent, not an array, or names an unknown lane.
bool parseFeedbackLanes(const Json& body, std::vector<FeedbackLane>& out, std::string& bad);

FeedbackTestPlan planFeedbackTest(uint16_t caps, const std::vector<FeedbackLane>& requested);

// The Controller::testHeldLanes bits a burst of `sent` holds.
uint8_t heldFeedbackLanes(const std::vector<FeedbackLane>& sent);

RumbleReport testRumbleReport();
TriggerEffectsReport testTriggerEffectsReport();

} // namespace satellite
