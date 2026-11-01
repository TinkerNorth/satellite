// SPDX-License-Identifier: LGPL-3.0-or-later

#include "mic_worker.h"

#include "core/tracked_mutex.h"

#include <chrono>

namespace {

constexpr int NAME_READ_ATTEMPTS = 64;

void bump(std::atomic<uint64_t>& counter) { counter.fetch_add(1, std::memory_order_relaxed); }

} // namespace

MicWorker::MicWorker(IGamepadPort& backend, ILogPort& log, const HotTable& hot, RxEpoch& rx,
                     IAudioCodecFactory* codecs)
    : backend_(backend), log_(log), hot_(hot), rx_(rx), codecs_(codecs) {
    thread_ = std::thread([this] { run(); });
}

MicWorker::~MicWorker() {
    stopping_.store(true, std::memory_order_seq_cst);
    wake_.notify();
    if (thread_.joinable()) thread_.join();
}

MicPacket* MicWorker::claim() { return packets_->claim(); }

void MicWorker::publish() {
    packets_->publish();
    wake_.notify();
}

void MicWorker::noteRejected() { bump(late_); }

MicCounts MicWorker::counts() const {
    MicCounts c;
    c.accepted = accepted_.load(std::memory_order_relaxed);
    c.late = late_.load(std::memory_order_relaxed);
    c.decoded = decoded_.load(std::memory_order_relaxed);
    c.fecRecovered = fecRecovered_.load(std::memory_order_relaxed);
    c.concealed = concealed_.load(std::memory_order_relaxed);
    return c;
}

void MicWorker::run() {
    ScopedThreadRole role(ThreadRole::Mic);
    for (;;) {
        bool worked = false;
        while (const MicPacket* packet = packets_->front()) {
            process(*packet);
            packets_->release();
            packetsDone_.fetch_add(1, std::memory_order_release);
            worked = true;
        }
        if (worked) continue;
        const uint64_t asked = sweepsAsked_.load(std::memory_order_seq_cst);
        sweep();
        sweepsDone_.store(asked, std::memory_order_seq_cst);
        if (stopping_.load(std::memory_order_seq_cst)) return;
        wake_.sleepUnless([this] { return hasWork(); });
    }
}

bool MicWorker::hasWork() const {
    return !packets_->empty() ||
           sweepsAsked_.load(std::memory_order_seq_cst) !=
               sweepsDone_.load(std::memory_order_seq_cst) ||
           stopping_.load(std::memory_order_seq_cst);
}

void MicWorker::process(const MicPacket& packet) {
    Pad& pad = pads_[packet.serial];
    if (pad.gen != packet.gen) {
        pad.gen = packet.gen;
        pad.slot = packet.slot;
        pad.ctrlIdx = packet.ctrlIdx;
        pad.window.reset();
        pad.decoder.reset();
    }
    const AudioJitterWindow::Result pushed =
        pad.window.push(packet.seq, packet.bytes, packet.length);
    if (pushed.accept != AudioJitterWindow::Accept::Ok) {
        bump(late_);
        lastAccepted_.store(false, std::memory_order_release);
        return;
    }
    bump(accepted_);
    lastAccepted_.store(true, std::memory_order_release);
    if (pushed.count == 0) return;

    if (!pad.decoder && codecs_ != nullptr) pad.decoder = codecs_->makeMicDecoder();
    if (!pad.decoder) return;

    int16_t pcm[AUDIO_FRAME_SAMPLES * AUDIO_MIC_CHANNELS];
    for (int i = 0; i < pushed.count; i++) {
        const AudioJitterWindow::Event& ev = pushed.events[i];
        size_t decoded = 0;
        if (ev.kind == AudioJitterWindow::Event::Kind::Packet) {
            decoded = pad.decoder->decode(ev.data, ev.len, pcm, AUDIO_FRAME_SAMPLES);
            if (decoded > 0) bump(decoded_);
        } else if (ev.fecCarrier != nullptr) {
            decoded =
                pad.decoder->decodeFec(ev.fecCarrier, ev.fecCarrierLen, pcm, AUDIO_FRAME_SAMPLES);
            if (decoded > 0) bump(fecRecovered_);
        } else {
            decoded = pad.decoder->conceal(pcm, AUDIO_FRAME_SAMPLES);
            if (decoded > 0) bump(concealed_);
        }
        if (decoded == 0) continue;
        const Delivery delivery = submit(packet, pcm, decoded);
        if (delivery == Delivery::Unbound) return;
        if (delivery == Delivery::NoSink) noteNoSink(packet);
    }
}

MicWorker::Delivery MicWorker::submit(const MicPacket& packet, const int16_t* pcm, size_t frames) {
    rx_.enter(RX_MIC_PARTICIPANT);
    Delivery delivery = Delivery::Unbound;
    if (binding(packet.slot, packet.ctrlIdx, packet.serial, packet.gen) == Binding::Bound) {
        delivery = backend_.submitMicAudioPcm(packet.serial, pcm, frames) ? Delivery::Submitted
                                                                          : Delivery::NoSink;
    }
    rx_.exit(RX_MIC_PARTICIPANT);
    return delivery;
}

MicWorker::Binding MicWorker::binding(int slot, uint8_t ctrlIdx, uint32_t serial,
                                      uint32_t gen) const {
    if (slot < 0 || slot >= MAX_SESSIONS || ctrlIdx >= MAX_CONTROLLERS_PER_CONN) {
        return Binding::Unbound;
    }
    const HotSlot& hot = hot_.slots[static_cast<size_t>(slot)];
    uint32_t word = 0;
    uint32_t padGen = 0;
    const bool stable = hot.read([&] {
        word = hot.ctrl[ctrlIdx].load(std::memory_order_relaxed);
        padGen = hot.padGen[ctrlIdx].load(std::memory_order_relaxed);
    });
    if (!stable) return Binding::Changing;
    const bool bound =
        (word & HOT_CTRL_ACTIVE) != 0 && (word & HOT_CTRL_SERIAL_MASK) == serial && padGen == gen;
    return bound ? Binding::Bound : Binding::Unbound;
}

void MicWorker::sweep() {
    for (uint32_t serial = 1; serial <= static_cast<uint32_t>(MAX_BACKEND_CONTROLLERS); serial++) {
        Pad& pad = pads_[serial];
        if (pad.gen == 0) continue;
        if (binding(pad.slot, pad.ctrlIdx, serial, pad.gen) != Binding::Unbound) continue;
        pad.gen = 0;
        pad.window.reset();
        pad.decoder.reset();
    }
}

bool MicWorker::deviceName(int slot, uint32_t keyEpoch, std::string& out) const {
    const HotSlot& hot = hot_.slots[static_cast<size_t>(slot)];
    for (int attempt = 0; attempt < NAME_READ_ATTEMPTS; attempt++) {
        uint32_t epoch = 0;
        const bool stable = hot.read([&] {
            epoch = hot.keyEpoch.load(std::memory_order_relaxed);
            out = hot.loadName();
        });
        if (stable) return epoch == keyEpoch;
        std::this_thread::yield();
    }
    return false;
}

void MicWorker::noteNoSink(const MicPacket& packet) {
    Session& s = sessions_[static_cast<size_t>(packet.slot)];
    if (s.keyEpoch != packet.keyEpoch) {
        s.keyEpoch = packet.keyEpoch;
        s.noSinkLogged = false;
    }
    if (s.noSinkLogged) return;
    std::string name;
    if (!deviceName(packet.slot, packet.keyEpoch, name)) return;
    s.noSinkLogged = true;
    log_.logMsg(LogLevel::INFO, "service",
                "Mic audio from " + name + " controller #" + std::to_string(packet.ctrlIdx) +
                    " decodes but the emulated pad has no microphone endpoint");
}

#ifdef SATELLITE_BUILD_TESTS
bool MicWorker::drainForTest() {
    constexpr auto DRAIN_LIMIT = std::chrono::seconds(10);
    const uint64_t packets = packets_->pushed();
    const uint64_t sweep = sweepsAsked_.fetch_add(1, std::memory_order_seq_cst) + 1;
    wake_.notify();
    const auto deadline = std::chrono::steady_clock::now() + DRAIN_LIMIT;
    while (packetsDone_.load(std::memory_order_acquire) < packets ||
           sweepsDone_.load(std::memory_order_acquire) < sweep) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    const uint64_t done = packetsDone_.load(std::memory_order_acquire);
    const bool processed = done != packetsAtLastDrain_;
    packetsAtLastDrain_ = done;
    return processed && lastAccepted_.load(std::memory_order_acquire);
}

std::thread::id MicWorker::threadIdForTest() const { return thread_.get_id(); }
#endif
