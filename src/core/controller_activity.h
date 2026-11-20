// SPDX-License-Identifier: LGPL-3.0-or-later

// What went into a virtual controller and what the game wrote out of it, as
// counts and the last value, never a stream (the records live in types.h, on
// Controller). Both diagnostics documents write it
// (docs/diagnostics-bundle.md "Every controller path, input and output").
#pragma once

#include "types.h"

#include <cstdint>

namespace satellite {

inline InputActivity startInputActivity(uint64_t counter, int64_t nowMs) {
    InputActivity a;
    a.baseline = counter;
    a.sampled = counter;
    a.sampledAtMs = nowMs;
    return a;
}

// A sample with no time elapsed (or a clock stepped back) keeps the last rate.
inline void sampleInputActivity(InputActivity& a, uint64_t counter, int64_t nowMs) {
    const int64_t elapsedMs = nowMs - a.sampledAtMs;
    if (elapsedMs <= 0) return;
    const uint64_t delta = counter - a.sampled;
    const uint64_t elapsed = static_cast<uint64_t>(elapsedMs);
    const uint64_t rounded = (delta * 1000u + elapsed / 2u) / elapsed;
    a.rateHz = static_cast<uint32_t>(rounded);
    if (delta > 0) a.lastAtMs = nowMs;
    a.sampled = counter;
    a.sampledAtMs = nowMs;
}

inline uint64_t inputFramesSince(const InputActivity& a, uint64_t counter) {
    return counter - a.baseline;
}

inline void noteOutputLane(OutputLane& lane, int64_t atMs) {
    lane.count++;
    lane.atMs = atMs;
}

inline void recordGameRumble(GameOutputRecord& r, const RumbleReport& report, int64_t atMs) {
    noteOutputLane(r.rumble, atMs);
    r.rumbleStrong = report.strongMagnitude;
    r.rumbleWeak = report.weakMagnitude;
}

inline void recordGameLightbar(GameOutputRecord& r, uint8_t red, uint8_t green, uint8_t blue,
                               int64_t atMs) {
    noteOutputLane(r.lightbar, atMs);
    r.lightbarR = red;
    r.lightbarG = green;
    r.lightbarB = blue;
}

inline void recordGameTriggerEffects(GameOutputRecord& r, const TriggerEffectsReport& report,
                                     int64_t atMs) {
    noteOutputLane(r.triggerEffects, atMs);
    r.triggers = report;
}

inline void recordGamePlayerLeds(GameOutputRecord& r, uint8_t mask, int64_t atMs) {
    noteOutputLane(r.playerLeds, atMs);
    r.playerLedsMask = mask;
}

inline void recordGameMicLed(GameOutputRecord& r, uint8_t state, int64_t atMs) {
    noteOutputLane(r.micLed, atMs);
    r.micLedState = state;
}

// The facts a diagnostics document writes as a controller's `input` block.
struct InputFacts {
    uint64_t frames = 0;
    uint32_t rateHz = 0;
    int64_t lastAtMs = 0;
};

inline InputFacts inputFacts(const InputActivity& a, uint64_t counter) {
    InputFacts f;
    f.frames = inputFramesSince(a, counter);
    f.rateHz = a.rateHz;
    f.lastAtMs = a.lastAtMs;
    return f;
}

} // namespace satellite
