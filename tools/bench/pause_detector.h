// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace satellite::bench {

struct Pause {
    double startMs = 0.0;
    double endMs = 0.0;

    double lengthMs() const { return endMs - startMs; }
};

enum class SlotChange { None, Connected, Disconnected };

struct SlotObservation {
    SlotChange change = SlotChange::None;
    std::optional<Pause> pause;
};

struct SlotSummary {
    uint64_t connects = 0;
    uint64_t updates = 0;
    double longestPauseMs = 0.0;
    double longestPauseEndMs = 0.0;
    uint64_t pausesOverThreshold = 0;
};

class PauseDetector {
  public:
    PauseDetector(size_t slotCount, double gapMs) : slots_(slotCount), gapMs_(gapMs) {}

    double gapMs() const { return gapMs_; }

    size_t slotCount() const { return slots_.size(); }

    SlotSummary summary(size_t slot) const {
        if (slot >= slots_.size()) return SlotSummary{};
        return slots_[slot].summary;
    }

    SlotObservation observe(size_t slot, bool connected, uint32_t packetNumber, double nowMs) {
        SlotObservation observation;
        if (slot >= slots_.size()) return observation;
        Slot& state = slots_[slot];
        observation.change = applyConnection(state, connected);
        if (!connected) return observation;
        if (!state.baselined) {
            baseline(state, packetNumber, nowMs);
            return observation;
        }
        if (packetNumber == state.lastPacket) return observation;
        observation.pause = recordUpdate(state, packetNumber, nowMs);
        return observation;
    }

  private:
    struct Slot {
        bool connected = false;
        bool baselined = false;
        uint32_t lastPacket = 0;
        double lastChangeMs = 0.0;
        SlotSummary summary;
    };

    static SlotChange applyConnection(Slot& state, bool connected) {
        if (connected == state.connected) return SlotChange::None;
        state.connected = connected;
        if (!connected) return SlotChange::Disconnected;
        state.baselined = false;
        state.summary.connects += 1;
        return SlotChange::Connected;
    }

    static void baseline(Slot& state, uint32_t packetNumber, double nowMs) {
        state.baselined = true;
        state.lastPacket = packetNumber;
        state.lastChangeMs = nowMs;
    }

    std::optional<Pause> recordUpdate(Slot& state, uint32_t packetNumber, double nowMs) const {
        const double sinceChangeMs = nowMs - state.lastChangeMs;
        const Pause pause{state.lastChangeMs, nowMs};
        const bool overThreshold = sinceChangeMs >= gapMs_;
        const bool longest = sinceChangeMs > state.summary.longestPauseMs;
        if (longest) {
            state.summary.longestPauseMs = sinceChangeMs;
            state.summary.longestPauseEndMs = nowMs;
        }
        if (overThreshold) state.summary.pausesOverThreshold += 1;
        state.summary.updates += 1;
        state.lastPacket = packetNumber;
        state.lastChangeMs = nowMs;
        if (!overThreshold) return std::nullopt;
        return pause;
    }

    std::vector<Slot> slots_;
    const double gapMs_;
};

} // namespace satellite::bench
