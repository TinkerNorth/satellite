// SPDX-License-Identifier: LGPL-3.0-or-later

// core/controller_activity: a pad's input count turned into frames, a rate and
// a last arrival, and every game output report counted per lane with its last
// value.
#include "../src/core/controller_activity.h"

#include <cstdint>

#include "test_util.h"

using namespace satellite;

static void test_input_counts_from_the_plug() {
    TEST("a pad counts the frames since it was plugged, not the serial's whole history");
    const InputActivity a = startInputActivity(5000, 1000);
    EXPECT_EQ(inputFramesSince(a, 5000), uint64_t{0});
    EXPECT_EQ(inputFramesSince(a, 5250), uint64_t{250});
    EXPECT_EQ(a.rateHz, 0u);
    EXPECT_EQ(a.lastAtMs, int64_t{0});
}

static void test_input_rate_per_sample() {
    TEST("a sample a second later turns the frames in between into a rate in Hz");
    InputActivity a = startInputActivity(0, 1000);
    sampleInputActivity(a, 250, 2000);
    EXPECT_EQ(a.rateHz, 250u);
    EXPECT_EQ(a.lastAtMs, int64_t{2000});

    TEST("an uneven period is rounded to the nearest whole Hz");
    sampleInputActivity(a, 250 + 120, 2000 + 800);
    EXPECT_EQ(a.rateHz, 150u);

    TEST("a sample that saw no frame reads 0 Hz and keeps the last arrival");
    sampleInputActivity(a, 370, 3800);
    EXPECT_EQ(a.rateHz, 0u);
    EXPECT_EQ(a.lastAtMs, int64_t{2800});
    EXPECT_EQ(inputFramesSince(a, 370), uint64_t{370});
}

static void test_input_sample_without_time() {
    TEST("a sample with no time elapsed, or a clock stepped back, keeps the last rate");
    InputActivity a = startInputActivity(0, 1000);
    sampleInputActivity(a, 100, 2000);
    sampleInputActivity(a, 200, 2000);
    EXPECT_EQ(a.rateHz, 100u);
    sampleInputActivity(a, 300, 1500);
    EXPECT_EQ(a.rateHz, 100u);
    EXPECT_EQ(a.sampled, uint64_t{100});
}

static void test_input_facts() {
    TEST("the input facts are the frames since the plug, the last rate and the last arrival");
    InputActivity a = startInputActivity(10, 0);
    sampleInputActivity(a, 70, 1000);
    const InputFacts f = inputFacts(a, 95);
    EXPECT_EQ(f.frames, uint64_t{85});
    EXPECT_EQ(f.rateHz, 60u);
    EXPECT_EQ(f.lastAtMs, int64_t{1000});
}

static void test_output_lanes() {
    TEST("each output lane counts every report the game wrote and keeps the last value");
    GameOutputRecord r;
    RumbleReport rumble;
    rumble.strongMagnitude = 32768;
    rumble.weakMagnitude = 7;
    recordGameRumble(r, rumble, 100);
    rumble.strongMagnitude = 1;
    recordGameRumble(r, rumble, 200);
    EXPECT_EQ(r.rumble.count, uint64_t{2});
    EXPECT_EQ(r.rumble.atMs, int64_t{200});
    EXPECT_EQ(r.rumbleStrong, uint16_t{1});
    EXPECT_EQ(r.rumbleWeak, uint16_t{7});

    recordGameLightbar(r, 0, 0, 255, 300);
    EXPECT_EQ(r.lightbar.count, uint64_t{1});
    EXPECT_EQ(r.lightbarB, uint8_t{255});
    EXPECT_EQ(r.lightbar.atMs, int64_t{300});

    TriggerEffectsReport triggers;
    triggers.left[0] = 0x01;
    recordGameTriggerEffects(r, triggers, 400);
    EXPECT_EQ(r.triggerEffects.count, uint64_t{1});
    EXPECT(r.triggers == triggers);

    recordGamePlayerLeds(r, 0x04, 500);
    EXPECT_EQ(r.playerLeds.count, uint64_t{1});
    EXPECT_EQ(r.playerLedsMask, uint8_t{0x04});

    recordGameMicLed(r, MIC_LED_STATE_PULSE, 600);
    EXPECT_EQ(r.micLed.count, uint64_t{1});
    EXPECT_EQ(r.micLedState, MIC_LED_STATE_PULSE);

    TEST("a lane the game never wrote stays at zero");
    const GameOutputRecord empty;
    EXPECT_EQ(empty.rumble.count, uint64_t{0});
    EXPECT_EQ(empty.micLed.atMs, int64_t{0});
}

int main() {
    return satellite::test::runMain("controller_activity", [] {
        test_input_counts_from_the_plug();
        test_input_rate_per_sample();
        test_input_sample_without_time();
        test_input_facts();
        test_output_lanes();
    });
}
