// SPDX-License-Identifier: LGPL-3.0-or-later

// core/audio/audio_resampler — exact-rational sample-rate conversion between a
// composite persona's own USB-audio endpoints and the pinned wire rate.
//
// Counting output samples is not enough here: a resampler that drops the
// lowpass still produces the right COUNT and sounds like aliasing mush, and one
// that mishandles the polyphase index still produces the right count and sounds
// like a chipmunk. So the suite measures what the filter actually did — tone
// energy in band, tone energy out of band, and frequency identified by zero
// crossings — plus the streaming invariant that chunking must not change the
// samples.
#include "test_util.h"

#include "core/audio/audio_resampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using satellite::audio::RationalResampler;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Interleaved tone generator: `chAmp[c]` scales channel c, so a test can put a
// tone on one channel and silence on another.
std::vector<int16_t> tone(int rateHz, double freqHz, size_t frames,
                          const std::vector<double>& chAmp, double phase0 = 0.0) {
    std::vector<int16_t> out(frames * chAmp.size());
    for (size_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / rateHz;
        const double s = std::sin(2.0 * kPi * freqHz * t + phase0);
        for (size_t c = 0; c < chAmp.size(); ++c) {
            out[f * chAmp.size() + c] = static_cast<int16_t>(s * chAmp[c] * 20000.0);
        }
    }
    return out;
}

// RMS of one channel of an interleaved buffer, skipping `skipFrames` at the
// front so a filter's fill-in transient never colours the measurement.
double rms(const std::vector<int16_t>& pcm, size_t channels, size_t channel, size_t skipFrames) {
    const size_t frames = pcm.size() / channels;
    if (frames <= skipFrames) return 0.0;
    double acc = 0.0;
    for (size_t f = skipFrames; f < frames; ++f) {
        const double v = pcm[f * channels + channel];
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<double>(frames - skipFrames));
}

// Dominant frequency by counting rising zero crossings. Crude but exactly the
// property that catches a wrong-rate stream: a 1 kHz tone resampled with the
// wrong ratio comes out at some other frequency, and nothing about its energy
// would say so.
double zeroCrossFreq(const std::vector<int16_t>& pcm, size_t channels, size_t channel, int rateHz,
                     size_t skipFrames) {
    const size_t frames = pcm.size() / channels;
    if (frames <= skipFrames + 1) return 0.0;
    int crossings = 0;
    for (size_t f = skipFrames + 1; f < frames; ++f) {
        const int16_t prev = pcm[(f - 1) * channels + channel];
        const int16_t cur = pcm[f * channels + channel];
        if (prev < 0 && cur >= 0) crossings++;
    }
    const double seconds = static_cast<double>(frames - skipFrames - 1) / rateHz;
    return seconds > 0.0 ? crossings / seconds : 0.0;
}

std::vector<int16_t> runAll(RationalResampler& rs, const std::vector<int16_t>& in,
                            size_t channels) {
    std::vector<int16_t> out;
    rs.process(in.data(), in.size() / channels, out);
    return out;
}

} // namespace

static void test_identity_passthrough() {
    TEST("equal rates are the identity: bytes pass through untouched");
    RationalResampler rs;
    rs.configure(48000, 48000, 2);
    EXPECT(rs.identity());
    const std::vector<int16_t> in = tone(48000, 1000.0, 480, {1.0, 0.5});
    const std::vector<int16_t> out = runAll(rs, in, 2);
    EXPECT_EQ(out.size(), in.size());
    EXPECT(out == in);

    TEST("a nonsensical rate configures the identity rather than dividing by zero");
    RationalResampler bad;
    bad.configure(0, 48000, 1);
    EXPECT(bad.identity());
    bad.configure(48000, -1, 1);
    EXPECT(bad.identity());
}

static void test_upsample_32k_to_48k() {
    TEST("32 kHz -> 48 kHz (the DualShock 4 v2 speaker endpoint): 3 out per 2 in");
    RationalResampler rs;
    rs.configure(32000, 48000, 2);
    EXPECT(!rs.identity());
    // 3200 frames = 100 ms, long enough that the 16-tap fill-in is noise.
    const std::vector<int16_t> in = tone(32000, 1000.0, 3200, {1.0, 1.0});
    const std::vector<int16_t> out = runAll(rs, in, 2);
    const size_t outFrames = out.size() / 2;
    EXPECT(outFrames >= 4790 && outFrames <= 4801);

    TEST("32 kHz -> 48 kHz keeps the tone at 1 kHz and keeps its level");
    EXPECT(std::fabs(zeroCrossFreq(out, 2, 0, 48000, 200) - 1000.0) < 20.0);
    const double inRms = rms(in, 2, 0, 200);
    const double outRms = rms(out, 2, 0, 200);
    EXPECT(outRms > inRms * 0.9 && outRms < inRms * 1.1);
}

static void test_downsample_48k_to_16k() {
    TEST("48 kHz -> 16 kHz (the DualShock 4 v2 mic endpoint): 1 out per 3 in");
    RationalResampler rs;
    rs.configure(48000, 16000, 1);
    const std::vector<int16_t> in = tone(48000, 1000.0, 9600, {1.0});
    const std::vector<int16_t> out = runAll(rs, in, 1);
    EXPECT(out.size() >= 3195 && out.size() <= 3201);

    TEST("48 kHz -> 16 kHz keeps an in-band tone at its own frequency and level");
    EXPECT(std::fabs(zeroCrossFreq(out, 1, 0, 16000, 100) - 1000.0) < 20.0);
    const double inRms = rms(in, 1, 0, 100);
    const double outRms = rms(out, 1, 0, 100);
    EXPECT(outRms > inRms * 0.9 && outRms < inRms * 1.1);
}

// The whole reason this is a filter and not a stride: decimating 48 kHz by 3
// without a lowpass folds 15 kHz down to 1 kHz, right into the voice band,
// where it would be loud and obviously wrong. A stride implementation passes
// every count-based assertion and fails this one.
static void test_downsample_rejects_out_of_band_energy() {
    TEST("48 kHz -> 16 kHz attenuates a 15 kHz tone instead of aliasing it in");
    RationalResampler rs;
    rs.configure(48000, 16000, 1);
    const std::vector<int16_t> in = tone(48000, 15000.0, 9600, {1.0});
    const std::vector<int16_t> out = runAll(rs, in, 1);
    const double inRms = rms(in, 1, 0, 100);
    const double outRms = rms(out, 1, 0, 100);
    EXPECT(outRms < inRms * 0.05); // better than -26 dB of what a stride would pass

    TEST("the same rate pair still passes a 3 kHz tone at full level");
    RationalResampler pass;
    pass.configure(48000, 16000, 1);
    const std::vector<int16_t> inPass = tone(48000, 3000.0, 9600, {1.0});
    const std::vector<int16_t> outPass = runAll(pass, inPass, 1);
    const double passIn = rms(inPass, 1, 0, 100);
    const double passOut = rms(outPass, 1, 0, 100);
    EXPECT(passOut > passIn * 0.85);
}

// A backend hands over batch boundaries, not signal boundaries, so the same
// signal chunked differently must produce the same samples. A resampler that
// forgets its history or its phase between calls fails here and nowhere else.
static void test_streaming_matches_one_shot() {
    TEST("chunked input produces byte-identical output to one-shot input");
    const std::vector<int16_t> in = tone(48000, 700.0, 2400, {1.0, 1.0});

    RationalResampler oneShot;
    oneShot.configure(48000, 32000, 2);
    const std::vector<int16_t> whole = runAll(oneShot, in, 2);

    RationalResampler chunked;
    chunked.configure(48000, 32000, 2);
    std::vector<int16_t> pieces;
    const size_t chunkFrames = 37; // deliberately not a multiple of the ratio
    for (size_t f = 0; f < 2400; f += chunkFrames) {
        const size_t n = std::min(chunkFrames, size_t(2400) - f);
        chunked.process(in.data() + f * 2, n, pieces);
    }
    EXPECT_EQ(pieces.size(), whole.size());
    EXPECT(pieces == whole);

    TEST("process appends rather than assigns, so a caller can accumulate");
    RationalResampler acc;
    acc.configure(48000, 48000, 1);
    std::vector<int16_t> sink{1, 2, 3};
    const std::vector<int16_t> more{4, 5};
    acc.process(more.data(), more.size(), sink);
    EXPECT_EQ(sink.size(), (size_t)5);
    EXPECT_EQ(sink[0], (int16_t)1);
    EXPECT_EQ(sink[4], (int16_t)5);
}

static void test_clear_drops_history() {
    TEST("clear() makes the next call behave like a fresh stream (replug)");
    const std::vector<int16_t> in = tone(48000, 700.0, 600, {1.0});

    RationalResampler fresh;
    fresh.configure(48000, 32000, 1);
    const std::vector<int16_t> first = runAll(fresh, in, 1);

    RationalResampler reused;
    reused.configure(48000, 32000, 1);
    std::vector<int16_t> discard;
    reused.process(in.data(), 600, discard); // dirty the filter history
    reused.clear();
    const std::vector<int16_t> second = runAll(reused, in, 1);
    EXPECT_EQ(second.size(), first.size());
    EXPECT(second == first);

    TEST("configure() also resets, so re-plugging at a new rate cannot leak audio");
    reused.configure(48000, 32000, 1);
    const std::vector<int16_t> third = runAll(reused, in, 1);
    EXPECT(third == first);
}

static void test_channels_are_independent() {
    TEST("stereo channels do not bleed into one another");
    RationalResampler rs;
    rs.configure(32000, 48000, 2);
    const std::vector<int16_t> in = tone(32000, 1200.0, 3200, {1.0, 0.0});
    const std::vector<int16_t> out = runAll(rs, in, 2);
    EXPECT(rms(out, 2, 0, 200) > 5000.0);
    EXPECT(rms(out, 2, 1, 200) < 1.0);

    TEST("channels() reports what was configured");
    EXPECT_EQ(rs.channels(), 2);
}

static void test_bounds_and_saturation() {
    TEST("maxOutputFrames is an upper bound on what process appends");
    RationalResampler rs;
    rs.configure(32000, 48000, 2);
    std::vector<int16_t> out;
    const std::vector<int16_t> in = tone(32000, 1000.0, 960, {1.0, 1.0});
    rs.process(in.data(), 960, out);
    EXPECT(out.size() / 2 <= rs.maxOutputFrames(960));

    TEST("a zero-length batch is a no-op, not an underflow");
    const size_t before = out.size();
    rs.process(in.data(), 0, out);
    EXPECT_EQ(out.size(), before);

    TEST("full-scale input clamps instead of wrapping to the opposite sign");
    RationalResampler loud;
    loud.configure(32000, 48000, 1);
    std::vector<int16_t> square(2000);
    for (size_t i = 0; i < square.size(); ++i)
        square[i] = (i / 8) % 2 ? static_cast<int16_t>(-32768) : static_cast<int16_t>(32767);
    std::vector<int16_t> loudOut;
    loud.process(square.data(), square.size(), loudOut);
    bool signFlipSpike = false;
    for (size_t i = 200; i + 1 < loudOut.size(); ++i) {
        // Overshoot is expected from a windowed-sinc; a WRAP shows up as a
        // sample near the opposite rail next to one near this rail.
        if (loudOut[i] > 30000 && loudOut[i + 1] < -30000) {
            // Legitimate only where the input actually flipped, which at this
            // period cannot happen on consecutive output samples.
            signFlipSpike = true;
            break;
        }
    }
    EXPECT(!signFlipSpike);
}

int main() {
    test_identity_passthrough();
    test_upsample_32k_to_48k();
    test_downsample_48k_to_16k();
    test_downsample_rejects_out_of_band_energy();
    test_streaming_matches_one_shot();
    test_clear_drops_history();
    test_channels_are_independent();
    test_bounds_and_saturation();

    std::cout << "audio_resampler: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
