// SPDX-License-Identifier: LGPL-3.0-or-later

// The haptic-waveform-to-motor-strength reduction (core/audio/haptic_envelope.h).
// What matters: the scale (a full-scale sine reads full magnitude), the lane
// mapping (left lane to the strong motor), exact zero for digital silence and
// for anything under the floor, and that the two lanes never bleed into each
// other. A reduction that quietly halved every level would still "work" and
// would make every haptic game feel weak, so the numbers are pinned.

#include "test_util.h"

#include "core/audio/haptic_envelope.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace satellite;
using namespace satellite::audio;

namespace {

// One 20 ms stereo window: independent sines per lane at the given peaks.
std::vector<int16_t> window(double leftPeak, double rightPeak, double hz = 200.0) {
    std::vector<int16_t> pcm(static_cast<size_t>(AUDIO_FRAME_SAMPLES) * AUDIO_HAPTIC_CHANNELS);
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        const double t = static_cast<double>(i) / AUDIO_SAMPLE_RATE_HZ;
        pcm[static_cast<size_t>(i) * 2 + 0] =
            static_cast<int16_t>(leftPeak * std::sin(2.0 * 3.14159265358979 * hz * t));
        pcm[static_cast<size_t>(i) * 2 + 1] =
            static_cast<int16_t>(rightPeak * std::sin(2.0 * 3.14159265358979 * hz * 1.5 * t));
    }
    return pcm;
}

} // namespace

static void test_full_scale_sine_reads_full_magnitude() {
    TEST("a full-scale sine on a lane reads (nearly) 65535");
    const auto pcm = window(32767.0, 32767.0);
    const RumbleReport r = hapticWindowToRumble(pcm.data(), AUDIO_FRAME_SAMPLES);
    // Sine RMS over a non-integer number of cycles is not exactly peak/sqrt 2;
    // within 2 % of full scale is the pinned promise.
    EXPECT(r.strongMagnitude >= 64200);
    EXPECT(r.weakMagnitude >= 64200);

    TEST("a full-scale square wave saturates rather than wraps");
    std::vector<int16_t> square(static_cast<size_t>(AUDIO_FRAME_SAMPLES) * 2);
    for (size_t i = 0; i < square.size(); i++) square[i] = (i / 96 % 2 == 0) ? 32767 : -32767;
    const RumbleReport s = hapticWindowToRumble(square.data(), AUDIO_FRAME_SAMPLES);
    EXPECT_EQ(s.strongMagnitude, UINT16_MAX);
    EXPECT_EQ(s.weakMagnitude, UINT16_MAX);
}

static void test_scale_is_linear_in_amplitude() {
    TEST("half amplitude reads half magnitude");
    const RumbleReport full =
        hapticWindowToRumble(window(32000.0, 32000.0).data(), AUDIO_FRAME_SAMPLES);
    const RumbleReport half =
        hapticWindowToRumble(window(16000.0, 16000.0).data(), AUDIO_FRAME_SAMPLES);
    const RumbleReport tenth =
        hapticWindowToRumble(window(3200.0, 3200.0).data(), AUDIO_FRAME_SAMPLES);
    const double halfRatio = static_cast<double>(half.strongMagnitude) / full.strongMagnitude;
    const double tenthRatio = static_cast<double>(tenth.strongMagnitude) / full.strongMagnitude;
    EXPECT(halfRatio > 0.49 && halfRatio < 0.51);
    EXPECT(tenthRatio > 0.09 && tenthRatio < 0.11);
}

static void test_lanes_map_to_motors_and_stay_independent() {
    TEST("left lane drives the strong motor, right lane the weak, independently");
    const RumbleReport leftOnly =
        hapticWindowToRumble(window(24000.0, 0.0).data(), AUDIO_FRAME_SAMPLES);
    EXPECT(leftOnly.strongMagnitude > 40000);
    EXPECT_EQ(leftOnly.weakMagnitude, uint16_t{0});

    const RumbleReport rightOnly =
        hapticWindowToRumble(window(0.0, 24000.0).data(), AUDIO_FRAME_SAMPLES);
    EXPECT_EQ(rightOnly.strongMagnitude, uint16_t{0});
    EXPECT(rightOnly.weakMagnitude > 40000);

    TEST("the caller never sets a duration here");
    EXPECT_EQ(leftOnly.durationMs, uint16_t{0});
}

static void test_silence_and_floor() {
    TEST("digital silence reduces to exact zero");
    const std::vector<int16_t> silence(static_cast<size_t>(AUDIO_FRAME_SAMPLES) * 2, 0);
    const RumbleReport r = hapticWindowToRumble(silence.data(), AUDIO_FRAME_SAMPLES);
    EXPECT_EQ(r.strongMagnitude, uint16_t{0});
    EXPECT_EQ(r.weakMagnitude, uint16_t{0});

    TEST("dither-level noise is under the floor and reads zero");
    std::vector<int16_t> dither(static_cast<size_t>(AUDIO_FRAME_SAMPLES) * 2);
    for (size_t i = 0; i < dither.size(); i++) dither[i] = static_cast<int16_t>((i % 3) - 1);
    const RumbleReport d = hapticWindowToRumble(dither.data(), AUDIO_FRAME_SAMPLES);
    EXPECT_EQ(d.strongMagnitude, uint16_t{0});
    EXPECT_EQ(d.weakMagnitude, uint16_t{0});

    TEST("the first level over the floor is the floor, not a jump");
    // -40 dBFS RMS on a sine is a peak of ~463; sweep across the threshold.
    uint16_t below = 1;
    uint16_t above = 0;
    for (double peak = 300.0; peak < 700.0; peak += 10.0) {
        const RumbleReport s = hapticWindowToRumble(window(peak, 0.0).data(), AUDIO_FRAME_SAMPLES);
        if (s.strongMagnitude == 0) {
            below = 0;
        } else {
            above = s.strongMagnitude;
            break;
        }
    }
    EXPECT_EQ(below, uint16_t{0});
    EXPECT(above >= HAPTIC_RUMBLE_FLOOR);
    EXPECT(above < HAPTIC_RUMBLE_FLOOR + 40);
}

static void test_lane_magnitude_bounds() {
    TEST("hapticLaneMagnitude refuses bad arguments with zero");
    const auto pcm = window(20000.0, 20000.0);
    EXPECT_EQ(hapticLaneMagnitude(nullptr, AUDIO_FRAME_SAMPLES, 2, 0), uint16_t{0});
    EXPECT_EQ(hapticLaneMagnitude(pcm.data(), 0, 2, 0), uint16_t{0});
    EXPECT_EQ(hapticLaneMagnitude(pcm.data(), AUDIO_FRAME_SAMPLES, 0, 0), uint16_t{0});
    EXPECT_EQ(hapticLaneMagnitude(pcm.data(), AUDIO_FRAME_SAMPLES, 2, 2), uint16_t{0});
    EXPECT_EQ(hapticLaneMagnitude(pcm.data(), AUDIO_FRAME_SAMPLES, 2, -1), uint16_t{0});

    TEST("a shorter window is measured over what it holds");
    const uint16_t whole = hapticLaneMagnitude(pcm.data(), AUDIO_FRAME_SAMPLES, 2, 0);
    const uint16_t partial = hapticLaneMagnitude(pcm.data(), AUDIO_FRAME_SAMPLES / 2, 2, 0);
    // Same sine, half the frames: same RMS to within a couple of percent.
    const double ratio = static_cast<double>(partial) / whole;
    EXPECT(ratio > 0.95 && ratio < 1.05);
}

int main() {
    test_full_scale_sine_reads_full_magnitude();
    test_scale_is_linear_in_amplitude();
    test_lanes_map_to_motors_and_stay_independent();
    test_silence_and_floor();
    test_lane_magnitude_bounds();

    std::cout << "haptic_envelope: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
