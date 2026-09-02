// SPDX-License-Identifier: LGPL-3.0-or-later

// The libopus wrapper (adapters/audio/opus_codec.*): the two controller-audio
// stream formats, round-tripped through real encoders and decoders.
//
// The assertions are deliberately about SHAPE rather than samples. Opus is
// lossy and version-dependent, so pinning bytes would pin the library version;
// what must not drift is that a 20 ms window in comes back out as a 20 ms
// window, that a tone survives as a tone, that concealment produces audio for a
// frame that never arrived, and above all that the mic stream really carries
// in-band FEC -- which is an encoder-setting question, and exactly the sort of
// thing that silently stops being true.
#include "../src/adapters/audio/opus_codec.h"
#include "../src/core/types.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "test_util.h"

namespace {

using satellite::audio::OpusCodecFactory;
using satellite::audio::OpusStreamDecoder;
using satellite::audio::OpusStreamEncoder;
using satellite::audio::Stream;

const size_t MIC_FRAME = static_cast<size_t>(AUDIO_FRAME_SAMPLES);
const size_t SPEAKER_FRAME = static_cast<size_t>(AUDIO_FRAME_SAMPLES) * AUDIO_SPEAKER_CHANNELS;
// Comfortably past a 96 kbps 20 ms packet (~240 bytes) without being the wire
// ceiling, so a wildly oversized packet would still be visible as one.
const size_t MAX_PACKET = 1024;

// Speech-ish content: a 220 Hz fundamental plus two harmonics, amplitude
// modulated so successive frames differ. Steady silence would let a
// concealment path look identical to a real decode and prove nothing.
void fillMicFrame(std::vector<int16_t>& pcm, int frameIndex) {
    pcm.resize(MIC_FRAME);
    for (size_t i = 0; i < MIC_FRAME; i++) {
        const double t = (frameIndex * static_cast<double>(MIC_FRAME) + static_cast<double>(i)) /
                         AUDIO_SAMPLE_RATE_HZ;
        const double env = 0.55 + 0.45 * std::sin(2.0 * 3.14159265358979 * 3.0 * t);
        const double s = std::sin(2.0 * 3.14159265358979 * 220.0 * t) +
                         0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 * t) +
                         0.25 * std::sin(2.0 * 3.14159265358979 * 880.0 * t);
        pcm[i] = static_cast<int16_t>(env * s * 8000.0);
    }
}

// Stereo with the channels deliberately unequal, so a wrapper that collapsed or
// swapped them would show up as an energy imbalance rather than passing.
void fillSpeakerFrame(std::vector<int16_t>& pcm, int frameIndex) {
    pcm.resize(SPEAKER_FRAME);
    for (size_t i = 0; i < MIC_FRAME; i++) {
        const double t = (frameIndex * static_cast<double>(MIC_FRAME) + static_cast<double>(i)) /
                         AUDIO_SAMPLE_RATE_HZ;
        const double l = std::sin(2.0 * 3.14159265358979 * 330.0 * t);
        const double r = std::sin(2.0 * 3.14159265358979 * 660.0 * t);
        pcm[i * 2 + 0] = static_cast<int16_t>(l * 9000.0);
        pcm[i * 2 + 1] = static_cast<int16_t>(r * 4000.0);
    }
}

// Mean square over a channel of an interleaved buffer (stride 1 for mono).
double energy(const int16_t* pcm, size_t frames, int stride, int offset) {
    if (frames == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < frames; i++) {
        const double v = pcm[i * stride + offset];
        sum += v * v;
    }
    return sum / static_cast<double>(frames);
}

bool sameSamples(const std::vector<int16_t>& a, const std::vector<int16_t>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

void test_micRoundTrip_preservesFrameAndSignal() {
    TEST("opus mic: 20 ms mono in, 20 ms mono out, and the tone survives");
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    auto dec = OpusStreamDecoder::create(Stream::Mic);
    EXPECT(enc != nullptr);
    EXPECT(dec != nullptr);
    if (!enc || !dec) return;
    EXPECT_EQ(enc->channels(), AUDIO_MIC_CHANNELS);
    EXPECT_EQ(dec->channels(), AUDIO_MIC_CHANNELS);

    std::vector<int16_t> src;
    std::vector<int16_t> out(MIC_FRAME * 2, 0);
    uint8_t packet[MAX_PACKET];
    double lastEnergyRatio = 0.0;

    // Several frames: Opus needs a few to leave its start-up transient, and a
    // wrapper that only worked on frame 0 would be a real bug.
    for (int f = 0; f < 12; f++) {
        fillMicFrame(src, f);
        const size_t bytes = enc->encode(src.data(), MIC_FRAME, packet, sizeof(packet));
        EXPECT(bytes > 0);
        EXPECT(bytes < sizeof(packet));
        const size_t frames = dec->decode(packet, bytes, out.data(), out.size());
        EXPECT_EQ(frames, MIC_FRAME);
        if (f >= 4) {
            const double in = energy(src.data(), MIC_FRAME, 1, 0);
            const double got = energy(out.data(), MIC_FRAME, 1, 0);
            lastEnergyRatio = in > 0.0 ? got / in : 0.0;
            // Lossy, so not equal; but a codec that dropped the signal or blew
            // it up by an order of magnitude is broken, not lossy.
            EXPECT(lastEnergyRatio > 0.3);
            EXPECT(lastEnergyRatio < 3.0);
        }
    }
    EXPECT(lastEnergyRatio > 0.0);

    // ~32 kbps at 20 ms is ~80 bytes; the assertion is only that VBR is not
    // running an order of magnitude off the configured rate.
    fillMicFrame(src, 20);
    const size_t bytes = enc->encode(src.data(), MIC_FRAME, packet, sizeof(packet));
    EXPECT(bytes > 20);
    EXPECT(bytes < 400);
}

void test_speakerRoundTrip_keepsBothChannels() {
    TEST("opus speaker: stereo round-trips with the channel imbalance intact");
    auto enc = OpusStreamEncoder::create(Stream::Speaker);
    auto dec = OpusStreamDecoder::create(Stream::Speaker);
    EXPECT(enc != nullptr);
    EXPECT(dec != nullptr);
    if (!enc || !dec) return;
    EXPECT_EQ(enc->channels(), AUDIO_SPEAKER_CHANNELS);
    EXPECT_EQ(dec->channels(), AUDIO_SPEAKER_CHANNELS);

    std::vector<int16_t> src;
    std::vector<int16_t> out(SPEAKER_FRAME * 2, 0);
    uint8_t packet[MAX_PACKET];
    double leftOverRight = 0.0;
    for (int f = 0; f < 12; f++) {
        fillSpeakerFrame(src, f);
        const size_t bytes = enc->encode(src.data(), MIC_FRAME, packet, sizeof(packet));
        EXPECT(bytes > 0);
        const size_t frames = dec->decode(packet, bytes, out.data(), out.size() / 2);
        EXPECT_EQ(frames, MIC_FRAME);
        if (f >= 4) {
            const double l = energy(out.data(), MIC_FRAME, 2, 0);
            const double r = energy(out.data(), MIC_FRAME, 2, 1);
            leftOverRight = r > 0.0 ? l / r : 0.0;
        }
    }
    // Source left is ~2.25x right in power. A wrapper that downmixed to mono,
    // or swapped the interleave, would land near 1.0 or well under it.
    EXPECT(leftOverRight > 1.5);
    EXPECT(leftOverRight < 6.0);
}

void test_encodeRejectsWrongWindow() {
    TEST("opus: a window that is not exactly 20 ms is refused, not silently reframed");
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    EXPECT(enc != nullptr);
    if (!enc) return;

    std::vector<int16_t> src;
    fillMicFrame(src, 0);
    uint8_t packet[MAX_PACKET];
    // One wire message is one 20 ms packet: a caller handing a different window
    // has mis-framed, and emitting the packet anyway would put audio on the
    // wire the receiver cannot place in its timeline.
    EXPECT_EQ(enc->encode(src.data(), MIC_FRAME - 1, packet, sizeof(packet)), (size_t)0);
    EXPECT_EQ(enc->encode(src.data(), MIC_FRAME + 1, packet, sizeof(packet)), (size_t)0);
    EXPECT_EQ(enc->encode(src.data(), 0, packet, sizeof(packet)), (size_t)0);
    EXPECT_EQ(enc->encode(nullptr, MIC_FRAME, packet, sizeof(packet)), (size_t)0);
    EXPECT_EQ(enc->encode(src.data(), MIC_FRAME, nullptr, sizeof(packet)), (size_t)0);
    EXPECT_EQ(enc->encode(src.data(), MIC_FRAME, packet, 0), (size_t)0);
    // And the good call still works afterwards: a refusal must not wedge the
    // encoder.
    EXPECT(enc->encode(src.data(), MIC_FRAME, packet, sizeof(packet)) > 0);
}

void test_decodeRejectsMalformedInput() {
    TEST("opus: garbage and truncated packets fail cleanly, leaving a usable decoder");
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    auto dec = OpusStreamDecoder::create(Stream::Mic);
    EXPECT(enc != nullptr);
    EXPECT(dec != nullptr);
    if (!enc || !dec) return;

    std::vector<int16_t> out(MIC_FRAME * 2, 0);
    const uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    // Not asserting failure: some byte strings ARE valid Opus. Asserting only
    // that nothing reads out of bounds and the decoder survives.
    (void)dec->decode(garbage, sizeof(garbage), out.data(), out.size());
    EXPECT_EQ(dec->decode(nullptr, 4, out.data(), out.size()), (size_t)0);
    EXPECT_EQ(dec->decode(garbage, 0, out.data(), out.size()), (size_t)0);
    EXPECT_EQ(dec->decode(garbage, sizeof(garbage), nullptr, out.size()), (size_t)0);
    EXPECT_EQ(dec->decode(garbage, sizeof(garbage), out.data(), 0), (size_t)0);

    std::vector<int16_t> src;
    fillMicFrame(src, 0);
    uint8_t packet[MAX_PACKET];
    const size_t bytes = enc->encode(src.data(), MIC_FRAME, packet, sizeof(packet));
    EXPECT(bytes > 4);
    (void)dec->decode(packet, bytes / 2, out.data(), out.size()); // truncated
    // Whatever the malformed input did, a real packet still decodes.
    EXPECT_EQ(dec->decode(packet, bytes, out.data(), out.size()), MIC_FRAME);
}

void test_decodeRefusesAPacketLongerThanTheWireWindow() {
    TEST("opus: a packet claiming more than 20 ms is refused, never written past the buffer");
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    auto dec = OpusStreamDecoder::create(Stream::Mic);
    EXPECT(enc != nullptr);
    EXPECT(dec != nullptr);
    if (!enc || !dec) return;

    std::vector<int16_t> src;
    fillMicFrame(src, 0);
    uint8_t packet[MAX_PACKET];
    const size_t bytes = enc->encode(src.data(), MIC_FRAME, packet, sizeof(packet));
    EXPECT(bytes > 1);
    if (bytes <= 1) return;

    // Forge a 40 ms packet out of the 20 ms one: Opus's TOC byte carries the
    // frame count in its low two bits, so code 1 (two equal frames) plus a
    // duplicated body is a structurally valid packet of twice the duration.
    // This is exactly what a hostile client could put on the wire, and the
    // caller decodes into a fixed AUDIO_FRAME_SAMPLES buffer, so "refused" and
    // "not written past" have to be the same thing.
    std::vector<uint8_t> twoFrames;
    twoFrames.push_back(static_cast<uint8_t>((packet[0] & 0xFC) | 0x01));
    twoFrames.insert(twoFrames.end(), packet + 1, packet + bytes);
    twoFrames.insert(twoFrames.end(), packet + 1, packet + bytes);

    std::vector<int16_t> out(MIC_FRAME, 0);
    EXPECT_EQ(dec->decode(twoFrames.data(), twoFrames.size(), out.data(), MIC_FRAME), (size_t)0);
    // Given room for the whole 40 ms it decodes fine, which is what makes the
    // refusal above a capacity check rather than the packet being malformed.
    std::vector<int16_t> roomy(MIC_FRAME * 2, 0);
    EXPECT_EQ(dec->decode(twoFrames.data(), twoFrames.size(), roomy.data(), MIC_FRAME * 2),
              MIC_FRAME * 2);
}

void test_conceal_producesAFrameForNothing() {
    TEST("opus: concealment synthesizes a full frame with no packet at all");
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    auto dec = OpusStreamDecoder::create(Stream::Mic);
    EXPECT(enc != nullptr);
    EXPECT(dec != nullptr);
    if (!enc || !dec) return;

    std::vector<int16_t> src;
    std::vector<int16_t> out(MIC_FRAME, 0);
    uint8_t packet[MAX_PACKET];
    for (int f = 0; f < 8; f++) {
        fillMicFrame(src, f);
        const size_t bytes = enc->encode(src.data(), MIC_FRAME, packet, sizeof(packet));
        EXPECT_EQ(dec->decode(packet, bytes, out.data(), out.size()), MIC_FRAME);
    }

    std::vector<int16_t> concealed(MIC_FRAME, 0);
    EXPECT_EQ(dec->conceal(concealed.data(), concealed.size()), MIC_FRAME);
    // Extrapolated from the tone that came before, so it must not be silence.
    // (An all-zero frame is what a decoder that ignored the request would give.)
    EXPECT(energy(concealed.data(), MIC_FRAME, 1, 0) > 1000.0);

    // Too small a buffer is a caller error, not a request to conceal less: the
    // codec has to be told exactly how much audio is missing.
    EXPECT_EQ(dec->conceal(concealed.data(), MIC_FRAME - 1), (size_t)0);
    EXPECT_EQ(dec->conceal(nullptr, MIC_FRAME), (size_t)0);
}

// Encode a run of mic frames, drop `lost`, and decode the run twice from
// identical decoder state: once recovering the hole from the carrier packet's
// in-band FEC, once concealing it blind. Whether the two outputs differ is
// exactly "did packet lost+1 carry a redundant copy of frame lost".
bool fecBeatsPlcForFrame(int lost, double& outFecEnergy, double& outSourceEnergy) {
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    auto decFec = OpusStreamDecoder::create(Stream::Mic);
    auto decPlc = OpusStreamDecoder::create(Stream::Mic);
    if (!enc || !decFec || !decPlc) return false;

    const int kFrames = 14;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<std::vector<int16_t>> sources;
    for (int f = 0; f < kFrames; f++) {
        std::vector<int16_t> src;
        fillMicFrame(src, f);
        uint8_t buf[MAX_PACKET];
        const size_t bytes = enc->encode(src.data(), MIC_FRAME, buf, sizeof(buf));
        if (bytes == 0) return false;
        packets.push_back(std::vector<uint8_t>(buf, buf + bytes));
        sources.push_back(src);
    }

    std::vector<int16_t> fecOut(MIC_FRAME, 0);
    std::vector<int16_t> plcOut(MIC_FRAME, 0);
    std::vector<int16_t> scratch(MIC_FRAME, 0);
    for (int f = 0; f < kFrames; f++) {
        if (f == lost) {
            // Recovered from packet f+1, which is what the jitter window hands
            // over as a gap's carrier. Order matters: the FEC copy is decoded
            // BEFORE the carrier's own frame.
            if (decFec->decodeFec(packets[f + 1].data(), packets[f + 1].size(), fecOut.data(),
                                  fecOut.size()) != MIC_FRAME) {
                return false;
            }
            if (decPlc->conceal(plcOut.data(), plcOut.size()) != MIC_FRAME) return false;
            continue;
        }
        decFec->decode(packets[f].data(), packets[f].size(), scratch.data(), scratch.size());
        decPlc->decode(packets[f].data(), packets[f].size(), scratch.data(), scratch.size());
    }

    outFecEnergy = energy(fecOut.data(), MIC_FRAME, 1, 0);
    outSourceEnergy = energy(sources[lost].data(), MIC_FRAME, 1, 0);
    return !sameSamples(fecOut, plcOut);
}

void test_fecRecoversALostFrame() {
    TEST("opus mic: FEC in the next packet recovers most lost frames; PLC alone cannot");
    // Every frame in a run, not one: whether a given packet carries LBRR is an
    // encoder decision made per packet, and a mode switch can make the two
    // decode paths diverge for a frame on its own. A strict majority separates
    // the two worlds cleanly -- measured at 7/8 with in-band FEC on and 2/8
    // with it off, which is the regression this pins. Nothing else would catch
    // it: a stream with no FEC encodes, decodes and sounds perfectly fine right
    // up until the first packet goes missing.
    const int first = 4;
    const int last = 11;
    const int trials = last - first + 1;
    int recovered = 0;
    double fecEnergy = 0.0;
    double sourceEnergy = 0.0;
    for (int lost = first; lost <= last; lost++) {
        double e = 0.0;
        double s = 0.0;
        if (fecBeatsPlcForFrame(lost, e, s)) {
            recovered++;
            fecEnergy = e;
            sourceEnergy = s;
        }
    }
    EXPECT(recovered * 2 > trials);
    if (recovered == 0) return;

    // And a recovery is audio, not a click: energy in the same league as what
    // was encoded for the frame that went missing.
    EXPECT(sourceEnergy > 0.0);
    EXPECT(fecEnergy > sourceEnergy * 0.1);
    EXPECT(fecEnergy < sourceEnergy * 10.0);
}

void test_decodeFecFallsBackToConcealment() {
    TEST("opus: decodeFec with no carrier conceals instead of failing");
    auto dec = OpusStreamDecoder::create(Stream::Mic);
    EXPECT(dec != nullptr);
    if (!dec) return;

    // Callers take the FEC path unconditionally on a gap, because whether a
    // packet carries FEC is an encoder decision they cannot see. A null carrier
    // therefore has to mean "conceal", not "fail".
    std::vector<int16_t> out(MIC_FRAME, 0);
    EXPECT_EQ(dec->decodeFec(nullptr, 0, out.data(), out.size()), MIC_FRAME);
    EXPECT_EQ(dec->decodeFec(nullptr, 0, out.data(), MIC_FRAME - 1), (size_t)0);
}

void test_factoryPinsTheStreamFormats() {
    TEST("opus factory: mic is the mono decoder, speaker is the stereo encoder");
    OpusCodecFactory factory;
    auto micDec = factory.makeMicDecoder();
    auto spkEnc = factory.makeSpeakerEncoder();
    EXPECT(micDec != nullptr);
    EXPECT(spkEnc != nullptr);
    if (!micDec || !spkEnc) return;

    // The formats the wire pins (types.h), reached through the seam the service
    // actually uses, so a factory wired to the wrong Stream fails here.
    std::vector<int16_t> mono(MIC_FRAME, 0);
    EXPECT_EQ(micDec->conceal(mono.data(), mono.size()), MIC_FRAME);

    std::vector<int16_t> stereo;
    fillSpeakerFrame(stereo, 0);
    uint8_t packet[MAX_PACKET];
    const size_t bytes = spkEnc->encode(stereo.data(), MIC_FRAME, packet, sizeof(packet));
    EXPECT(bytes > 0);

    // Decoding it as stereo yields a full window; the encoder really produced
    // two channels rather than a mono stream at the stereo bitrate.
    auto spkDec = OpusStreamDecoder::create(Stream::Speaker);
    EXPECT(spkDec != nullptr);
    if (!spkDec) return;
    std::vector<int16_t> out(SPEAKER_FRAME, 0);
    EXPECT_EQ(spkDec->decode(packet, bytes, out.data(), MIC_FRAME), MIC_FRAME);
}

void test_encoderRespectsTheOutputCeiling() {
    TEST("opus: a tight output buffer truncates the packet, it does not overrun it");
    auto enc = OpusStreamEncoder::create(Stream::Speaker);
    EXPECT(enc != nullptr);
    if (!enc) return;

    std::vector<int16_t> src;
    fillSpeakerFrame(src, 3);
    // libopus treats max_data_bytes as a hard ceiling it encodes down to, so a
    // small buffer produces a smaller packet rather than a buffer overrun. The
    // wire ceiling (MAX_INNER_PAYLOAD_BYTES) is generous, but the guarantee is
    // what makes passing sizeof(buffer) safe at the call site.
    uint8_t tight[64];
    const size_t bytes = enc->encode(src.data(), MIC_FRAME, tight, sizeof(tight));
    EXPECT(bytes > 0);
    EXPECT(bytes <= sizeof(tight));
}

// DTX is asymmetric on purpose, and the asymmetry is invisible from the header:
// only behaviour can pin it. The mic wants it because a live microphone never
// goes digitally silent, so a VAD gate is the only thing that can collapse a
// quiet room. The speaker must not have it, because its gate cuts anything
// ~26-30 dB below the recent peak -- on game audio that turns a reverb tail or
// quiet ambience into comfort noise.
void test_micEncoderUsesDtxOnSilence() {
    TEST("mic: sustained digital silence collapses to tiny DTX packets");
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    EXPECT(enc != nullptr);
    if (!enc) return;

    const std::vector<int16_t> silence(MIC_FRAME, 0);
    uint8_t packet[MAX_PACKET];

    // DTX needs a run of qualifying input before it engages (measured at 200 ms
    // on libopus 1.6.1), so the steady state is what gets asserted, not frame 1.
    size_t tiny = 0;
    size_t total = 0;
    for (int i = 0; i < 100; i++) {
        const size_t bytes = enc->encode(silence.data(), MIC_FRAME, packet, sizeof(packet));
        EXPECT(bytes > 0);
        if (i >= 20) {
            total++;
            if (bytes <= 2) tiny++;
        }
    }
    EXPECT(total > 0);
    EXPECT(tiny > total * 3 / 4);

    TEST("mic: DTX packets are still legal frames the decoder accepts");
    // A 1-byte packet is a valid Opus frame, not a malformed one -- the wire
    // minimum is header + 1 byte precisely so it survives dispatch.
    auto dec = OpusStreamDecoder::create(Stream::Mic);
    EXPECT(dec != nullptr);
    if (!dec) return;
    const size_t bytes = enc->encode(silence.data(), MIC_FRAME, packet, sizeof(packet));
    EXPECT(bytes >= 1);
    EXPECT(bytes <= 2);
    std::vector<int16_t> out(MIC_FRAME, 12345);
    EXPECT_EQ(dec->decode(packet, bytes, out.data(), AUDIO_FRAME_SAMPLES), MIC_FRAME);
}

void test_micEncoderKeepsSpeechIntact() {
    TEST("mic: DTX does not gate real speech");
    auto enc = OpusStreamEncoder::create(Stream::Mic);
    EXPECT(enc != nullptr);
    if (!enc) return;

    std::vector<int16_t> pcm(MIC_FRAME);
    uint8_t packet[MAX_PACKET];
    size_t tiny = 0;
    for (int i = 0; i < 60; i++) {
        fillMicFrame(pcm, i);
        const size_t bytes = enc->encode(pcm.data(), MIC_FRAME, packet, sizeof(packet));
        EXPECT(bytes > 0);
        if (bytes <= 2) tiny++;
    }
    EXPECT_EQ(tiny, (size_t)0);
}

void test_speakerEncoderDoesNotUseDtx() {
    TEST("speaker: silence stays a full packet, because DTX is deliberately off");
    auto enc = OpusStreamEncoder::create(Stream::Speaker);
    EXPECT(enc != nullptr);
    if (!enc) return;

    const std::vector<int16_t> silence(SPEAKER_FRAME, 0);
    uint8_t packet[MAX_PACKET];
    size_t tiny = 0;
    for (int i = 0; i < 100; i++) {
        const size_t bytes = enc->encode(silence.data(), SPEAKER_FRAME / AUDIO_SPEAKER_CHANNELS,
                                         packet, sizeof(packet));
        EXPECT(bytes > 0);
        if (bytes <= 2) tiny++;
    }
    // Not a bug being pinned: the speaker path suppresses exact silence before
    // it ever reaches the encoder (SessionService::sendSpeakerFrameLocked), so
    // the codec never needs a VAD and must not have one.
    EXPECT_EQ(tiny, (size_t)0);
}

} // namespace

int main() {
    std::cout << "Running Opus codec tests...\n\n";
    test_micRoundTrip_preservesFrameAndSignal();
    test_speakerRoundTrip_keepsBothChannels();
    test_encodeRejectsWrongWindow();
    test_decodeRejectsMalformedInput();
    test_decodeRefusesAPacketLongerThanTheWireWindow();
    test_conceal_producesAFrameForNothing();
    test_fecRecoversALostFrame();
    test_decodeFecFallsBackToConcealment();
    test_factoryPinsTheStreamFormats();
    test_encoderRespectsTheOutputCeiling();
    test_micEncoderUsesDtxOnSilence();
    test_micEncoderKeepsSpeechIntact();
    test_speakerEncoderDoesNotUseDtx();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
