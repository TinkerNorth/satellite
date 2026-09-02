// SPDX-License-Identifier: LGPL-3.0-or-later

// libopus behind the core codec seam (core/audio/audio_codec.h). Every knob the
// two controller-audio streams need is pinned in opus_codec.cpp; nothing about
// the format is negotiated at runtime, so this header is only about lifetime.
//
// Both directions of both streams are defined here even though the satellite
// only decodes mic and encodes speaker. The other two halves are what the
// client does, and having all four in one file is what lets a test close the
// loop on either stream instead of asserting against a second implementation
// of the same constants.
#pragma once

#include "core/audio/audio_codec.h"

#include <cstddef>
#include <cstdint>
#include <memory>

// libopus's handle types, forward-declared rather than #include <opus.h>: this
// header is included by the platform mains and by tests that have no reason to
// carry the codec's include path, and opus.h's own `typedef struct OpusEncoder
// OpusEncoder` agrees with these declarations.
struct OpusEncoder;
struct OpusDecoder;

namespace satellite::audio {

// Which of the two wire streams an instance is pinned to. The distinction is
// not just channel count: the mic runs Opus's VOIP application at a bitrate
// where in-band FEC exists, the speaker runs the AUDIO application at a bitrate
// where fidelity matters more (see opus_codec.cpp for the numbers and why).
enum class Stream { Mic, Speaker };

// Declared here, defined in opus_codec.cpp, so the unique_ptrs below work
// against the incomplete handle types above.
struct OpusEncoderDeleter {
    void operator()(::OpusEncoder* enc) const noexcept;
};
struct OpusDecoderDeleter {
    void operator()(::OpusDecoder* dec) const noexcept;
};

class OpusStreamDecoder : public IAudioDecoder {
  public:
    // Null when libopus refuses to allocate. Callers treat that as "no codec"
    // rather than fatal: a controller without audio is still a controller.
    static std::unique_ptr<OpusStreamDecoder> create(Stream stream);

    size_t decode(const uint8_t* opus, size_t opusLen, int16_t* pcm, size_t maxFrames) override;
    size_t conceal(int16_t* pcm, size_t maxFrames) override;
    size_t decodeFec(const uint8_t* opus, size_t opusLen, int16_t* pcm, size_t maxFrames) override;

    int channels() const { return channels_; }

  private:
    OpusStreamDecoder(std::unique_ptr<::OpusDecoder, OpusDecoderDeleter> dec, int channels)
        : dec_(std::move(dec)), channels_(channels) {}

    std::unique_ptr<::OpusDecoder, OpusDecoderDeleter> dec_;
    int channels_ = 1;
};

class OpusStreamEncoder : public IAudioEncoder {
  public:
    // Null when libopus refuses to allocate; see OpusStreamDecoder::create.
    static std::unique_ptr<OpusStreamEncoder> create(Stream stream);

    size_t encode(const int16_t* pcm, size_t frames, uint8_t* out, size_t maxOut) override;

    int channels() const { return channels_; }

  private:
    OpusStreamEncoder(std::unique_ptr<::OpusEncoder, OpusEncoderDeleter> enc, int channels)
        : enc_(std::move(enc)), channels_(channels) {}

    std::unique_ptr<::OpusEncoder, OpusEncoderDeleter> enc_;
    int channels_ = 2;
};

// What the platform mains hand SessionService. Stateless: one instance for the
// process, outliving the service that borrows it.
class OpusCodecFactory : public IAudioCodecFactory {
  public:
    std::unique_ptr<IAudioDecoder> makeMicDecoder() override;
    std::unique_ptr<IAudioEncoder> makeSpeakerEncoder() override;
};

} // namespace satellite::audio
