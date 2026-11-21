// SPDX-License-Identifier: LGPL-3.0-or-later
#include "feedback_test.h"

#include <algorithm>

namespace satellite {

const char* feedbackLaneName(FeedbackLane lane) {
    for (const FeedbackLaneInfo& info : FEEDBACK_LANES) {
        if (info.lane == lane) return info.name;
    }
    return "";
}

bool parseFeedbackLane(const std::string& name, FeedbackLane& out) {
    for (const FeedbackLaneInfo& info : FEEDBACK_LANES) {
        if (name == info.name) {
            out = info.lane;
            return true;
        }
    }
    return false;
}

bool parseFeedbackLanes(const Json& body, std::vector<FeedbackLane>& out, std::string& bad) {
    out.clear();
    const auto lanes = body.find("lanes");
    if (lanes == body.end() || !lanes->is_array()) {
        bad = "lanes";
        return false;
    }
    for (const Json& name : *lanes) {
        FeedbackLane lane = FeedbackLane::Rumble;
        const bool known = name.is_string() && parseFeedbackLane(name.get<std::string>(), lane);
        if (!known) {
            bad = name.is_string() ? name.get<std::string>() : name.dump();
            return false;
        }
        out.push_back(lane);
    }
    return true;
}

FeedbackTestPlan planFeedbackTest(uint16_t caps, const std::vector<FeedbackLane>& requested) {
    FeedbackTestPlan plan;
    for (const FeedbackLaneInfo& info : FEEDBACK_LANES) {
        const bool asked =
            std::find(requested.begin(), requested.end(), info.lane) != requested.end();
        if (!asked) continue;
        const bool claimed = (caps & info.cap) != 0;
        if (claimed) {
            plan.sent.push_back(info.lane);
        } else {
            plan.skipped.push_back(info.lane);
        }
    }
    return plan;
}

RumbleReport testRumbleReport() {
    RumbleReport r;
    r.strongMagnitude = TEST_RUMBLE_MAGNITUDE;
    r.weakMagnitude = TEST_RUMBLE_MAGNITUDE;
    r.durationMs = TEST_RUMBLE_DURATION_MS;
    return r;
}

TriggerEffectsReport testTriggerEffectsReport() {
    TriggerEffectsReport t{};
    for (uint8_t* block : {t.left, t.right}) {
        block[0] = TEST_TRIGGER_MODE_RESISTANCE;
        block[1] = TEST_TRIGGER_START;
        block[2] = TEST_TRIGGER_FORCE;
    }
    return t;
}

} // namespace satellite
