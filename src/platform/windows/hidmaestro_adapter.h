// SPDX-License-Identifier: LGPL-3.0-or-later
// Hot path: per-serial state is a flat array (no hash lookups); each slot owns
// the duplicated section view + event handles and a persistent payload buffer,
// so a submit is pack + one seqlock write + SetEvent — no allocations, no
// syscall beyond the doorbell, no managed code. The elevated device lifecycle
// (driver install, SwDevice creation, section creation, handle duplication)
// lives behind IHidMaestroProvisioner and only runs at plug/unplug time.
//
// Controller audio adds per-serial workers (the speaker ring drain, and on a
// DualSense the haptic ring drain) alongside the output-ring worker, with the
// same doorbell/cancel/join lifecycle. Rate conversion between the persona's
// endpoints and the pinned wire rate happens here rather than in the helper,
// because here it is unit-tested; see hidmaestro_audio_wire.h for what
// crosses the rings.
#pragma once

#include "core/audio/audio_resampler.h"
#include "core/gamepad_backend.h"
#include "core/hidmaestro_version.h"
#include "core/ports.h"
#include "core/tracked_mutex.h"
#include "hidmaestro_audio_wire.h"
#include "hidmaestro_provisioner.h"
#include "pointer_inject.h"
#include "hidmaestro_report.h"
#include "hidmaestro_wire.h"

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// How long a Sony pad may sit without a frame before the maintenance tick
// re-publishes its last state. A real DualSense reports every 4 ms whether or
// not anything moved; Sony's own pad library (libScePad, which games like 007
// First Light drive the DualSense through) opens a pad by waiting for its
// next input report and reads one whose reports stop as gone, while every
// Dish client sends INPUT on change only. 100 ms keeps a pad alive at a rate
// nothing notices, and bounds the open wait to a tick or two. Only the Sony
// identities need it: XInput and the Switch stack keep state, not streams.
inline constexpr int SONY_IDLE_REPORT_MS = 100;

class HidMaestroAdapter : public IGamepadPort {
  public:
    // Read at every plug rather than cached, so flipping the `controllerAudio`
    // setting takes effect on the next pad without a restart and without this
    // adapter having to be told. Absent = audio off, which is the right
    // default for a decision that can install a kernel driver.
    using AudioEnabledFn = std::function<bool()>;

    // Fired either side of a plug that actually asked for an audio-carrying
    // persona, so the default-endpoint guard can snapshot before the endpoint
    // exists and start watching once it does.
    using CompositePlugHook = std::function<void()>;
    void setCompositePlugHooks(CompositePlugHook before, CompositePlugHook after);

    HidMaestroAdapter(satellite::hidmaestro::IHidMaestroProvisioner& provisioner, ILogPort& log,
                      AudioEnabledFn audioEnabled = {});
    ~HidMaestroAdapter() override;

    bool ensureBusOpen() override;
    void closeBus() override;
    bool isBusOpen() const override;
    bool pluginDevice(uint32_t serial, GamepadIdentity identity) override;
    bool supportsIdentity(GamepadIdentity identity) const override;
    const char* backendId() const override { return BACKEND_ID_HIDMAESTRO; }
    bool unplugDevice(uint32_t serial) override;
    bool isDevicePlugged(uint32_t serial) const override;
    bool submitReport(uint32_t serial, const GamepadReport& report) override;
    bool submitInput(uint32_t serial, const InputReport& input) override;
    // Re-publishes every plugged Sony pad that has gone SONY_IDLE_REPORT_MS
    // without a frame, clocks advanced, state unchanged (see the constant).
    void refreshIdleInput() override;
    void recheckDrivers() override;
    void setRumbleCallback(RumbleCallback cb) override;
    void setLightbarCallback(LightbarCallback cb) override;
    void setTriggerEffectsCallback(TriggerEffectsCallback cb) override;
    void setPlayerLedsCallback(PlayerLedsCallback cb) override;
    void setSpeakerAudioCallback(SpeakerAudioCallback cb) override;
    void setHapticAudioCallback(HapticAudioCallback cb) override;
    void setMicLedCallback(MicLedCallback cb) override;
    bool submitMicAudioPcm(uint32_t serial, const int16_t* mono48k, size_t samples) override;

    // True once this serial's persona actually presented the endpoint, i.e.
    // the composite materialized and the helper handed over the ring. Exposed
    // for tests and diagnostics; the wire caps come from the catalog.
    bool hasSpeakerEndpoint(uint32_t serial) const;
    bool hasMicEndpoint(uint32_t serial) const;
    bool hasHapticEndpoint(uint32_t serial) const;

    bool submitMotion(uint32_t serial, const MotionReport& report) override;
    bool submitBattery(uint32_t serial, const BatteryReport& report) override;
    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override;
    bool submitRelativeMouse(int dx, int dy, const MouseButtons& buttons, int wheelV) override;
    bool supportsRelativeMouse() const override { return true; }
    bool supportsMotionForType(uint8_t controllerType) const override;

#ifdef SATELLITE_BUILD_TESTS
    void setIdleReportIntervalForTest(std::chrono::milliseconds interval) {
        idleReportInterval_ = interval;
    }
    void setVersionDeadlineForTest(std::chrono::milliseconds deadline) {
        versionDeadline_ = deadline;
    }
#endif

  private:
    struct OutputWorker {
        std::thread th;
        HANDLE cancel = nullptr;
    };

    // Slot 0 unused so indexing matches wire-level serial numbers. Views and
    // handles are the duplicated objects the provisioner handed over; payload
    // persists across frames so a submit never allocates.
    struct IoSlot {
        GamepadIdentity identity = GamepadIdentity::Xbox;
        uint8_t* inputView = nullptr;
        const uint8_t* outputView = nullptr;
        HANDLE inputSection = nullptr;
        HANDLE inputEvent = nullptr;
        HANDLE companionEvent = nullptr;
        HANDLE outputSection = nullptr;
        HANDLE outputEvent = nullptr;

        // Controller-audio rings, all null on a persona with no audio function.
        // speakerView and hapticView are read-only (the helper produces),
        // micView is written. The haptic ring is null on every persona but the
        // DualSense composite; it shares the speaker's endpoint rate.
        const uint8_t* speakerView = nullptr;
        const uint8_t* hapticView = nullptr;
        uint8_t* micView = nullptr;
        HANDLE speakerSection = nullptr;
        HANDLE speakerEvent = nullptr;
        HANDLE hapticSection = nullptr;
        HANDLE hapticEvent = nullptr;
        HANDLE micSection = nullptr;
        HANDLE micEvent = nullptr;
        int speakerRateHz = 0;
        int micRateHz = 0;
        // 48 kHz mono from the wire down to whatever the persona's microphone
        // endpoint runs at. Lives on the slot (not the mic caller) because the
        // filter has history: chunking must not change the samples.
        satellite::audio::RationalResampler micResampler;
        std::vector<int16_t> micScratch;
        uint16_t micSeq = 0;

        // Merged latest-sample state per identity family, so motion/touchpad/
        // battery samples ride every subsequent frame (same model as the
        // ViGEm DS4 EX merge).
        Ds4InputState ds4{};
        satellite::hidmaestro::Ds5InputState ds5{};
        InputReport switchInput{};
        MotionReport switchMotion{};
        bool fingerDown0 = false;
        bool fingerDown1 = false;
        std::chrono::steady_clock::time_point lastSonySubmit{};

        uint8_t payload[satellite::hidmaestro::INPUT_DATA_CAPACITY]{};
        uint8_t gip[satellite::hidmaestro::INPUT_GIP_LENGTH]{};

        // Global\HIDMaestroVersion<N>, read-only, and what it must hold for
        // this pad to be written. The plug's check fills the rest before the
        // pad is published, the maintenance recheck after.
        const uint8_t* versionView = nullptr;
        HANDLE versionSection = nullptr;
        satellite::hidmaestro::VersionExpectation versionExpectation{};
        satellite::hidmaestro::SectionVersion loggedDriver{};
        std::chrono::steady_clock::time_point pendingSince{};

        // Atomic so isDevicePlugged can read without data races; written under
        // busMtx_ (plugin/unplug).
        std::atomic<bool> plugged{false};
        std::atomic_flag inputBusy = ATOMIC_FLAG_INIT;
        // What the version section allows, changed only while inputBusy is
        // held: input is written while Open, held while the driver's half is
        // still to come, and a Refused pad is inert both ways.
        std::atomic<satellite::hidmaestro::WriteGate> gate{satellite::hidmaestro::WriteGate::Open};
    };

    satellite::hidmaestro::IHidMaestroProvisioner& provisioner_;
    ILogPort& log_;
    AudioEnabledFn audioEnabled_;
    CompositePlugHook compositePlugBefore_;
    CompositePlugHook compositePlugAfter_;
    mutable TrackedMutex busMtx_{"hidmaestro.bus"};
    std::atomic<bool> busOpen_{false};
    std::chrono::milliseconds idleReportInterval_{SONY_IDLE_REPORT_MS};
    std::chrono::milliseconds versionDeadline_{satellite::hidmaestro::VERSION_HALF_DEADLINE_MS};

    std::array<IoSlot, MAX_BACKEND_CONTROLLERS + 1> io_;

    // Which helper-produced audio ring a drain worker reads.
    enum class AudioLane { Speaker, Haptic };

    // Plug/unplug-time only (not hot path), so a map is fine.
    std::unordered_map<uint32_t, OutputWorker> outputWorkers_;
    std::unordered_map<uint32_t, OutputWorker> speakerWorkers_;
    std::unordered_map<uint32_t, OutputWorker> hapticWorkers_;

    RumbleCallback rumbleCb_;
    LightbarCallback lightbarCb_;
    TriggerEffectsCallback triggerEffectsCb_;
    PlayerLedsCallback playerLedsCb_;
    SpeakerAudioCallback speakerAudioCb_;
    HapticAudioCallback hapticAudioCb_;
    MicLedCallback micLedCb_;

    RelMouseButtonState relMouseBtns_;

    void startOutputWorker(uint32_t serial); // caller holds busMtx_
    void stopOutputWorker(uint32_t serial);  // caller holds busMtx_
    void outputLoop(uint32_t serial, HANDLE cancel, uint32_t lastSeq);
    // outputLoop's delivery: the callbacks as registered at that moment, copied
    // out under busMtx_ so they run without it.
    struct OutputCallbacks {
        RumbleCallback rumble;
        LightbarCallback lightbar;
        TriggerEffectsCallback triggerEffects;
        PlayerLedsCallback playerLeds;
        MicLedCallback micLed;
    };
    OutputCallbacks snapshotOutputCallbacks() const;
    void deliverDecodedOutput(uint32_t serial, const satellite::hidmaestro::DecodedOutput& decoded,
                              TriggerEffectsReport& triggerEffects);
    void startAudioWorker(uint32_t serial, AudioLane lane); // caller holds busMtx_
    void stopAudioWorker(uint32_t serial, AudioLane lane);  // caller holds busMtx_
    void audioLoop(uint32_t serial, AudioLane lane, HANDLE cancel, uint32_t lastSeq);
    std::unordered_map<uint32_t, OutputWorker>& audioWorkers(AudioLane lane) {
        return lane == AudioLane::Speaker ? speakerWorkers_ : hapticWorkers_;
    }
    void releaseSlotLocked(IoSlot& slot);
    // Map the audio rings this plug came back with. Caller holds busMtx_;
    // failure is not fatal, it just leaves the pad without audio.
    void attachAudioLocked(IoSlot& slot, const satellite::hidmaestro::ProvisionResult& r);

    // Advance the Sony free-running clocks from wall time, pack the slot's
    // merged state for its identity, and publish the frame.
    bool packAndWrite(IoSlot& slot);
    // The slot's merged state packed as its identity's report into slot.payload:
    // the length, and whether the Xbox companion slice rides along.
    uint16_t packFrame(IoSlot& slot, bool& withGip);
    bool provisionSlot(uint32_t serial, GamepadIdentity identity, bool wantAudio,
                       satellite::hidmaestro::ProvisionResult& r);
    bool adoptProvisionedLocked(IoSlot& slot, const satellite::hidmaestro::ProvisionResult& r);
    void attachVersionLocked(IoSlot& slot, const satellite::hidmaestro::ProvisionResult& r);
    void startSlotWorkersLocked(uint32_t serial, const IoSlot& slot);

    // The plug's steps around the version check: adopt the helper's handles,
    // check the version section before the first write, then publish the pad
    // (open, or held) or hand the controller back.
    bool adoptSlot(uint32_t serial, GamepadIdentity identity,
                   const satellite::hidmaestro::ProvisionResult& r);
    bool admitSlot(uint32_t serial);
    bool publishSlot(uint32_t serial);
    void discardSlot(uint32_t serial);
    bool deliversFrom(uint32_t serial) const;

    void recheckSlotLocked(uint32_t serial, IoSlot& slot,
                           std::chrono::steady_clock::time_point now);
    void refuseSlotLocked(uint32_t serial, IoSlot& slot,
                          const satellite::hidmaestro::VersionCheck& check);
    void holdSlotLocked(uint32_t serial, IoSlot& slot);
    void openSlotLocked(uint32_t serial, IoSlot& slot);
    void noteDriver(uint32_t serial, IoSlot& slot,
                    const satellite::hidmaestro::VersionCheck& check);
    void logRefusal(uint32_t serial, const satellite::hidmaestro::VersionCheck& check,
                    const char* consequence) const;
};
