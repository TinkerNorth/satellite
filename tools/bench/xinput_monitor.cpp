// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "pause_detector.h"

#include <windows.h>

#include <xinput.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace {

using satellite::bench::PauseDetector;
using satellite::bench::SlotChange;
using satellite::bench::SlotObservation;
using satellite::bench::SlotSummary;

using Clock = std::chrono::steady_clock;
using WallClock = std::chrono::system_clock;
using Millis = std::chrono::duration<double, std::milli>;

constexpr size_t SLOT_COUNT = XUSER_MAX_COUNT;
constexpr double DEFAULT_SECS = 30.0;
constexpr double DEFAULT_GAP_MS = 40.0;
constexpr double MIN_SECS = 0.1;
constexpr double MIN_GAP_MS = 0.001;
constexpr double MILLIS_PER_SECOND = 1000.0;
constexpr int64_t MILLIS_PER_WHOLE_SECOND = 1000;
constexpr int USAGE_EXIT = 2;
constexpr int LOAD_FAILED_EXIT = 1;
constexpr const wchar_t* XINPUT_MODULE = L"xinput1_4.dll";
constexpr const char* XINPUT_GET_STATE = "XInputGetState";

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

struct Options {
    double secs = DEFAULT_SECS;
    double gapMs = DEFAULT_GAP_MS;
    bool help = false;
};

struct ParsedOptions {
    Options options;
    std::string error;
};

struct Timeline {
    Clock::time_point start;
    WallClock::time_point wallStart;
};

struct SweepStats {
    uint64_t sweeps = 0;
    double longestMs = 0.0;
};

bool parseAtLeast(const char* text, double minimum, double& out) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    const bool whole = end != text && *end == '\0';
    if (!whole || value < minimum) return false;
    out = value;
    return true;
}

bool applyValuedOption(Options& options, const std::string& name, const char* value) {
    if (name == "--secs") return parseAtLeast(value, MIN_SECS, options.secs);
    if (name == "--gap-ms") return parseAtLeast(value, MIN_GAP_MS, options.gapMs);
    return false;
}

ParsedOptions parseOptions(int argc, char** argv) {
    ParsedOptions parsed;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (name == "--help" || name == "-h") {
            parsed.options.help = true;
            continue;
        }
        const bool hasValue = i + 1 < argc;
        if (!hasValue) {
            parsed.error = "unknown option or missing value: " + name;
            return parsed;
        }
        const char* value = argv[++i];
        if (!applyValuedOption(parsed.options, name, value)) {
            parsed.error = "bad option: " + name + " " + value;
            return parsed;
        }
    }
    return parsed;
}

void printUsage(std::FILE* out) {
    std::fputs("usage: xinput_monitor [options]\n"
               "  --secs <s>     seconds to poll (default 30)\n"
               "  --gap-ms <ms>  report each pause of at least this long between two changes of "
               "a slot's\n"
               "                 packet number (default 40)\n",
               out);
}

template <typename Fn> Fn procAddress(HMODULE module, const char* name) {
    using Anything = void (*)();
    return reinterpret_cast<Fn>(reinterpret_cast<Anything>(GetProcAddress(module, name)));
}

GetStateFn loadGetState() {
    const HMODULE module = LoadLibraryExW(XINPUT_MODULE, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) return nullptr;
    return procAddress<GetStateFn>(module, XINPUT_GET_STATE);
}

double elapsedMs(const Timeline& timeline) { return Millis(Clock::now() - timeline.start).count(); }

std::string fixed(double value, int decimals) {
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), "%.*f", decimals, value);
    return text.data();
}

std::string wallClockText(WallClock::time_point wall) {
    const std::time_t seconds = WallClock::to_time_t(wall);
    std::tm local{};
    localtime_s(&local, &seconds);
    const auto sinceEpoch =
        std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch());
    const int64_t millis = sinceEpoch.count() % MILLIS_PER_WHOLE_SECOND;
    std::array<char, 32> date{};
    std::strftime(date.data(), date.size(), "%Y-%m-%dT%H:%M:%S", &local);
    std::array<char, 48> text{};
    std::snprintf(text.data(), text.size(), "%s.%03lld", date.data(),
                  static_cast<long long>(millis));
    return text.data();
}

std::string stampAt(const Timeline& timeline, double elapsed) {
    const auto offset = std::chrono::duration_cast<WallClock::duration>(Millis(elapsed));
    return wallClockText(timeline.wallStart + offset);
}

void printLine(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

std::string slotLabel(size_t slot) { return "slot " + std::to_string(slot); }

const char* changeText(SlotChange change) {
    switch (change) {
    case SlotChange::Connected:
        return "connected";
    case SlotChange::Disconnected:
        return "disconnected";
    case SlotChange::None:
        return "";
    }
    return "";
}

void report(const Timeline& timeline, size_t slot, const SlotObservation& seen, double nowMs) {
    if (seen.change != SlotChange::None) {
        printLine(stampAt(timeline, nowMs) + " " + slotLabel(slot) + " " + changeText(seen.change));
    }
    if (!seen.pause) return;
    const std::string from = stampAt(timeline, seen.pause->startMs);
    const std::string to = stampAt(timeline, seen.pause->endMs);
    printLine(from + " -> " + to + " " + slotLabel(slot) + " pause " +
              fixed(seen.pause->lengthMs(), 1) + " ms");
}

void pollSlot(const Timeline& timeline, GetStateFn getState, PauseDetector& detector, size_t slot) {
    XINPUT_STATE state{};
    const DWORD status = getState(static_cast<DWORD>(slot), &state);
    const double nowMs = elapsedMs(timeline);
    const bool connected = status == ERROR_SUCCESS;
    const SlotObservation seen = detector.observe(slot, connected, state.dwPacketNumber, nowMs);
    report(timeline, slot, seen, nowMs);
}

SweepStats pollUntil(const Timeline& timeline, GetStateFn getState, PauseDetector& detector,
                     double limitMs) {
    SweepStats stats;
    double previousStartMs = 0.0;
    for (double startMs = elapsedMs(timeline); startMs < limitMs; startMs = elapsedMs(timeline)) {
        if (stats.sweeps != 0)
            stats.longestMs = std::max(stats.longestMs, startMs - previousStartMs);
        previousStartMs = startMs;
        for (size_t slot = 0; slot < SLOT_COUNT; ++slot) {
            pollSlot(timeline, getState, detector, slot);
        }
        stats.sweeps += 1;
        YieldProcessor();
    }
    return stats;
}

void printSlotSummary(const Timeline& timeline, const PauseDetector& detector, size_t slot) {
    const SlotSummary summary = detector.summary(slot);
    if (summary.connects == 0) return;
    if (summary.updates == 0) {
        printLine(slotLabel(slot) + ": connected, no updates");
        return;
    }
    printLine(slotLabel(slot) + ": " + std::to_string(summary.updates) +
              " updates, longest pause " + fixed(summary.longestPauseMs, 1) + " ms ending " +
              stampAt(timeline, summary.longestPauseEndMs) + ", pauses >= " +
              fixed(detector.gapMs(), 1) + " ms: " + std::to_string(summary.pausesOverThreshold));
}

bool anySlotConnected(const PauseDetector& detector) {
    for (size_t slot = 0; slot < detector.slotCount(); ++slot) {
        if (detector.summary(slot).connects != 0) return true;
    }
    return false;
}

void printSummary(const Timeline& timeline, const PauseDetector& detector,
                  const SweepStats& stats) {
    if (!anySlotConnected(detector)) printLine("no controller was connected on any slot");
    for (size_t slot = 0; slot < detector.slotCount(); ++slot) {
        printSlotSummary(timeline, detector, slot);
    }
    printLine("polled " + std::to_string(stats.sweeps) + " sweeps, longest " +
              fixed(stats.longestMs, 1) + " ms between two readings of a slot");
}

int monitor(const Options& options, GetStateFn getState) {
    const Timeline timeline{Clock::now(), WallClock::now()};
    PauseDetector detector(SLOT_COUNT, options.gapMs);
    printLine(stampAt(timeline, 0.0) + " start, " + fixed(options.secs, 1) +
              " s, pauses of at least " + fixed(options.gapMs, 1) + " ms");
    const SweepStats stats =
        pollUntil(timeline, getState, detector, options.secs * MILLIS_PER_SECOND);
    printLine(stampAt(timeline, elapsedMs(timeline)) + " end");
    printSummary(timeline, detector, stats);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const ParsedOptions parsed = parseOptions(argc, argv);
    if (!parsed.error.empty()) {
        std::fprintf(stderr, "%s\n", parsed.error.c_str());
        printUsage(stderr);
        return USAGE_EXIT;
    }
    if (parsed.options.help) {
        printUsage(stdout);
        return 0;
    }
    const GetStateFn getState = loadGetState();
    if (getState == nullptr) {
        std::fprintf(stderr, "could not load XInputGetState from xinput1_4.dll\n");
        return LOAD_FAILED_EXIT;
    }
    return monitor(parsed.options, getState);
}
