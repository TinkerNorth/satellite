// SPDX-License-Identifier: LGPL-3.0-or-later

// Exact-rational sample-rate conversion for controller audio.
//
// Why this exists at all: the wire rate is pinned at AUDIO_SAMPLE_RATE_HZ, but
// an emulated pad's own USB-audio endpoints run at whatever the real hardware
// ran at. The DualSense composite persona is 48 kHz both ways and needs
// nothing; the DualShock 4 v2 persona is 32 kHz out / 16 kHz in, exactly like
// the pad it impersonates. Handing 32 kHz samples to a 48 kHz consumer plays
// the stream half again too fast, and decimating 48 kHz to 16 kHz without a
// lowpass folds everything above 8 kHz back into the voice band. Both are
// audible defects, not polish, so the conversion is a real polyphase FIR.
//
// Pure and dependency-free (same rule as audio_jitter.h) so the conversion is
// verified on every CI platform rather than only where the backend that needs
// it compiles.
//
// Structure: upsample by L, lowpass, decimate by M, with L/M = out/in reduced.
// The zero-stuffed samples are never materialized — output sample k sits at
// upsampled position u = k*M, whose phase u % L selects one of L tap subsets,
// and only that subset multiplies real input. Cost is therefore taps-per-phase
// multiplies per output sample regardless of L.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace satellite {
namespace audio {

// Taps per polyphase branch. 16 puts the transition band comfortably inside
// the guard the cutoff backs off to, at 16 multiplies per output sample per
// channel, which is nothing next to the Opus encode on the same thread.
inline constexpr int RESAMPLER_TAPS_PER_PHASE = 16;

// Backing off the cutoff from the theoretical Nyquist of the narrower rate
// leaves the FIR's transition band somewhere to live; 0.92 costs ~4% of the
// usable band and keeps the stopband where the aliases are.
inline constexpr float RESAMPLER_CUTOFF_FRACTION = 0.92f;

class RationalResampler {
  public:
    RationalResampler() = default;

    // (Re)configure and drop all history. Rates must be positive and channels
    // >= 1; anything else configures the identity, which passes samples
    // through untouched, so a caller never has to special-case a bad probe.
    void configure(int inRateHz, int outRateHz, int channels) {
        hist_.clear();
        pos_ = 0;
        channels_ = channels >= 1 ? channels : 1;
        if (inRateHz <= 0 || outRateHz <= 0 || inRateHz == outRateHz) {
            l_ = m_ = 1;
            taps_.clear();
            return;
        }
        const int g = gcd(inRateHz, outRateHz);
        l_ = outRateHz / g;
        m_ = inRateHz / g;
        buildFilter();
        hist_.assign(static_cast<size_t>(historyFrames()) * static_cast<size_t>(channels_), 0);
        pos_ = static_cast<int64_t>(historyFrames()) * l_;
    }

    bool identity() const { return l_ == 1 && m_ == 1; }
    int channels() const { return channels_; }

    // Forget filter history without re-deriving the coefficients: a replug is
    // a new stream and must not start with the tail of the old one.
    void clear() {
        std::fill(hist_.begin(), hist_.end(), static_cast<int16_t>(0));
        pos_ = static_cast<int64_t>(historyFrames()) * l_;
    }

    // Largest output frame count `process` can append for a given input, so
    // callers can size a buffer once instead of growing it per call. The +1
    // covers the sub-sample phase carried between calls.
    size_t maxOutputFrames(size_t inFrames) const {
        if (identity()) return inFrames;
        return (inFrames * static_cast<size_t>(l_)) / static_cast<size_t>(m_) + 1;
    }

    // Convert `frames` interleaved frames and APPEND the result to `out`
    // (appending, not assigning, so a caller can accumulate several calls into
    // one buffer). Streaming-safe: history and phase carry across calls, so
    // chunking the same signal differently produces the same samples.
    void process(const int16_t* in, size_t frames, std::vector<int16_t>& out) {
        const size_t ch = static_cast<size_t>(channels_);
        if (identity()) {
            out.insert(out.end(), in, in + frames * ch);
            return;
        }
        if (frames == 0) return;

        const size_t histFrames = static_cast<size_t>(historyFrames());
        work_.resize((histFrames + frames) * ch);
        if (histFrames > 0) std::copy(hist_.begin(), hist_.end(), work_.begin());
        std::copy(in, in + frames * ch, work_.begin() + static_cast<ptrdiff_t>(histFrames * ch));

        const int64_t total = static_cast<int64_t>(histFrames + frames);
        const int perPhase = RESAMPLER_TAPS_PER_PHASE;
        out.reserve(out.size() + maxOutputFrames(frames) * ch);
        while (pos_ / l_ <= total - 1) {
            const int phase = static_cast<int>(pos_ % l_);
            const int64_t newest = pos_ / l_; // youngest input frame this tap set reads
            for (size_t c = 0; c < ch; ++c) {
                float acc = 0.0f;
                for (int t = 0; t < perPhase; ++t) {
                    const int64_t idx = newest - t;
                    acc += taps_[static_cast<size_t>(phase * perPhase + t)] *
                           static_cast<float>(work_[static_cast<size_t>(idx) * ch + c]);
                }
                out.push_back(clampToI16(acc));
            }
            pos_ += m_;
        }

        // Keep exactly the frames a future tap set can still reach, and rebase
        // the phase onto the new buffer origin.
        if (histFrames > 0) {
            const size_t drop = frames * ch; // everything the new history replaces
            hist_.assign(work_.begin() + static_cast<ptrdiff_t>(drop), work_.end());
        }
        pos_ -= static_cast<int64_t>(frames) * l_;
    }

  private:
    static int gcd(int a, int b) {
        while (b != 0) {
            const int t = a % b;
            a = b;
            b = t;
        }
        return a;
    }

    static int16_t clampToI16(float v) {
        const float r = v >= 0.0f ? v + 0.5f : v - 0.5f;
        if (r > 32767.0f) return 32767;
        if (r < -32768.0f) return -32768;
        return static_cast<int16_t>(r);
    }

    // One tap set reads RESAMPLER_TAPS_PER_PHASE input frames ending at the
    // current one, so all but that current frame must survive to the next call.
    int historyFrames() const { return identity() ? 0 : RESAMPLER_TAPS_PER_PHASE - 1; }

    // Windowed-sinc prototype at the upsampled rate, stored phase-major so a
    // branch is contiguous. Cutoff is the narrower of the two Nyquists (the
    // decimator's, or the interpolator's image edge), backed off by
    // RESAMPLER_CUTOFF_FRACTION; the L gain restores what zero-stuffing took.
    void buildFilter() {
        const int perPhase = RESAMPLER_TAPS_PER_PHASE;
        const int n = perPhase * l_;
        const float fc = RESAMPLER_CUTOFF_FRACTION * 0.5f / static_cast<float>(l_ > m_ ? l_ : m_);
        const float center = static_cast<float>(n - 1) / 2.0f;
        std::vector<float> proto(static_cast<size_t>(n));
        const float pi = 3.14159265358979323846f;
        for (int i = 0; i < n; ++i) {
            const float x = static_cast<float>(i) - center;
            const float arg = 2.0f * pi * fc * x;
            const float sinc = (x == 0.0f) ? 2.0f * fc : std::sin(arg) / (pi * x);
            // Blackman: ~-58 dB stopband, which puts a folded image well under
            // the 16-bit floor for anything a controller headset carries.
            const float w =
                0.42f -
                0.5f * std::cos(2.0f * pi * static_cast<float>(i) / static_cast<float>(n - 1)) +
                0.08f * std::cos(4.0f * pi * static_cast<float>(i) / static_cast<float>(n - 1));
            proto[static_cast<size_t>(i)] = sinc * w * static_cast<float>(l_);
        }

        // Phase p, tap t reads input frame (pos/L - t), i.e. prototype index
        // p + t*L. Laying it out that way makes the inner loop a straight walk.
        taps_.assign(static_cast<size_t>(n), 0.0f);
        for (int p = 0; p < l_; ++p) {
            for (int t = 0; t < perPhase; ++t) {
                taps_[static_cast<size_t>(p * perPhase + t)] =
                    proto[static_cast<size_t>(p + t * l_)];
            }
        }
    }

    int l_ = 1;
    int m_ = 1;
    int channels_ = 1;
    int64_t pos_ = 0; // next output's position in upsampled samples
    std::vector<float> taps_;
    std::vector<int16_t> hist_;
    std::vector<int16_t> work_;
};

} // namespace audio
} // namespace satellite
