// SPDX-License-Identifier: LGPL-3.0-or-later

// The haptic waveform reduced to motor strength, for a client that cannot
// play the waveform itself.
//
// A DualSense game authors haptics as audio: two voice-coil lanes whose
// waveform IS the effect. A client with that pad on USB plays the lanes
// straight into it (CAP_HAPTIC_AUDIO). Every other pad has two plain motors,
// a phone has a vibrator, and the only thing they can render is "how hard",
// so the host reduces each 20 ms window to one magnitude per lane and sends
// it down the ordinary MSG_RUMBLE path. Left lane drives the strong motor,
// right the weak, which is the mapping the pad's own compatible-vibration
// mode uses, so a game that only ever speaks haptics still rumbles.
//
// RMS rather than peak: the motor is a low-pass device that renders the
// energy of a window, not its crest, and RMS is what a listener would call
// the level. It is scaled so a full-scale sine reads full magnitude
// (sine RMS = peak / sqrt 2), and a floor turns dither and reverb tails,
// which the pad would not have felt either, into exact zero rather than an
// endless faint buzz.
//
// Pure, dependency-free, so the reduction is unit-tested on every platform
// and not only where a composite pad exists.
#pragma once

#include "core/types.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace satellite::audio {

// -40 dBFS RMS. Quiet enough that nothing a game meant as an effect lands
// under it; the pad's own actuators are inaudible by then too.
inline constexpr uint16_t HAPTIC_RUMBLE_FLOOR = 655;

// How long one reduced window keeps a motor running on the client. Five
// frames of slack over the 20 ms cadence: a stream that stops mid-buzz (the
// game closed the endpoint, the packets got lost) stops the motor within
// this, and one lost packet does not.
inline constexpr uint16_t HAPTIC_RUMBLE_WIRE_DURATION_MS = 100;

// A steady level is re-sent at least this often while it is non-zero, so a
// sustained tone whose window RMS happens to quantize identically frame after
// frame does not coalesce away and let the client's timer run out.
inline constexpr int HAPTIC_RUMBLE_REFRESH_MS = 50;

// One lane's magnitude over one window of interleaved PCM: `channel` of
// `channels`, `frames` per channel. 0 exactly for digital silence and for
// anything under the floor.
inline uint16_t hapticLaneMagnitude(const int16_t* pcm, size_t frames, int channels, int channel) {
    if (pcm == nullptr || frames == 0 || channels <= 0 || channel < 0 || channel >= channels) {
        return 0;
    }
    double sumSquares = 0.0;
    for (size_t f = 0; f < frames; f++) {
        const double s = static_cast<double>(
            pcm[f * static_cast<size_t>(channels) + static_cast<size_t>(channel)]);
        sumSquares += s * s;
    }
    const double rms = std::sqrt(sumSquares / static_cast<double>(frames));
    // Full-scale sine (peak 32767, RMS 23170) maps to 65535.
    const double scaled = rms * std::sqrt(2.0) * (65535.0 / 32767.0);
    const uint16_t magnitude = scaled >= 65535.0 ? UINT16_MAX : static_cast<uint16_t>(scaled + 0.5);
    return magnitude < HAPTIC_RUMBLE_FLOOR ? 0 : magnitude;
}

// The whole window: left lane to the strong motor, right lane to the weak.
// durationMs is left for the caller, which knows what else is mixed in.
inline RumbleReport hapticWindowToRumble(const int16_t* stereo48k, size_t frames) {
    RumbleReport r;
    r.strongMagnitude = hapticLaneMagnitude(stereo48k, frames, AUDIO_HAPTIC_CHANNELS, 0);
    r.weakMagnitude = hapticLaneMagnitude(stereo48k, frames, AUDIO_HAPTIC_CHANNELS, 1);
    return r;
}

} // namespace satellite::audio
