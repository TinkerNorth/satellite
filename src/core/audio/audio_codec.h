// SPDX-License-Identifier: LGPL-3.0-or-later

// The codec seam for controller audio. Core owns the SHAPE of the two streams
// (formats and framing live in types.h); the library that actually codes them
// lives in adapters/audio/opus_codec.*, because src/core takes no third-party
// dependency and scripts/check_core_purity.sh enforces that.
//
// Same arrangement, and for the same reason, as SessionService::KeyDeriver:
// core describes what it needs, the shell hands it an implementation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// One controller's inbound stream. Stateful -- decoders carry filter state and
// a concealment history across frames -- so one instance per controller, never
// shared, and destroyed with the pad it belongs to.
//
// Every entry returns FRAMES (samples per channel) written, 0 on failure, and
// writes frames * channels interleaved int16 samples. `maxFrames` is capacity
// for decode(); for the two concealment entries it must be at least one whole
// AUDIO_FRAME_SAMPLES window, because the codec has to be told exactly how much
// audio is missing.
class IAudioDecoder {
  public:
    virtual ~IAudioDecoder() = default;
    virtual size_t decode(const uint8_t* opus, size_t opusLen, int16_t* pcm, size_t maxFrames) = 0;

    // No packet at all: synthesize one frame from the decoder's own history.
    virtual size_t conceal(int16_t* pcm, size_t maxFrames) = 0;

    // Recover the frame BEFORE `opus` from the in-band FEC copy that packet
    // carries. Degrades to plain concealment when it turns out to carry none,
    // so a caller never has to ask first (nor could it: whether a packet holds
    // FEC data is an encoder-side decision made per packet).
    virtual size_t decodeFec(const uint8_t* opus, size_t opusLen, int16_t* pcm,
                             size_t maxFrames) = 0;
};

// One controller's outbound stream. `frames` is per channel and must be exactly
// one AUDIO_FRAME_SAMPLES window: the wire carries one 20 ms packet per message
// and the windowing is the caller's job. Returns bytes written, 0 on failure.
class IAudioEncoder {
  public:
    virtual ~IAudioEncoder() = default;
    virtual size_t encode(const int16_t* pcm, size_t frames, uint8_t* out, size_t maxOut) = 0;
};

// Consulted lazily, on a controller's first audio frame in either direction, so
// a slot that advertises the caps but never speaks costs nothing. Null in a
// build or a suite with no codec wired: the audio paths then validate and drop,
// which is what they did before SAT-2 landed the codec.
class IAudioCodecFactory {
  public:
    virtual ~IAudioCodecFactory() = default;
    // Mono, mic format. Returns null when the codec refuses to allocate.
    virtual std::unique_ptr<IAudioDecoder> makeMicDecoder() = 0;
    // Stereo, speaker format. Returns null when the codec refuses to allocate.
    virtual std::unique_ptr<IAudioEncoder> makeSpeakerEncoder() = 0;
};
