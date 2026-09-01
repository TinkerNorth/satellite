// SPDX-License-Identifier: LGPL-3.0-or-later
// Hot path: per-serial state is a flat array (no hash lookups); each slot owns
// the duplicated section view + event handles and a persistent payload buffer,
// so a submit is pack + one seqlock write + SetEvent — no allocations, no
// syscall beyond the doorbell, no managed code. The elevated device lifecycle
// (driver install, SwDevice creation, section creation, handle duplication)
// lives behind IHidMaestroProvisioner and only runs at plug/unplug time.
//
// Locking: busMtx_ guards slot plugin/unplug, the worker maps, and the
// callbacks. Submits run entirely under it — the guarded section is a few
// hundred nanoseconds of memcpy into the mapped view (unlike ViGEm's blocking
// IOCTL there is nothing worth dropping the lock for), and holding it means a
// concurrent unplug can never unmap a view mid-write.
//
// Controller audio adds a second per-serial worker (the speaker ring drain)
// alongside the output-ring worker, with the same doorbell/cancel/join
// lifecycle. Rate conversion between the persona's endpoints and the pinned
// wire rate happens here rather than in the helper, because here it is
// unit-tested; see hidmaestro_audio_wire.h for what crosses the rings.
#pragma once

#include "core/audio/audio_resampler.h"
#include "core/gamepad_backend.h"
#include "core/ports.h"
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

class HidMaestroAdapter : public IGamepadPort {
  public:
    // Read at every plug rather than cached, so flipping the `controllerAudio`
    // setting takes effect on the next pad without a restart and without this
    // adapter having to be told. Absent = audio off, which is the right
    // default for a decision that can install a kernel driver.
    using AudioEnabledFn = std::function<bool()>;

    // Fired either side of a plug that actually asked for an audio-carrying
    // persona, so the default-endpoint guard can snapshot before the endpoint
    // exists and start watching once it does. Both run under the bus lock, so
    // the "after" hook must only signal, never wait.
    using CompositePlugHook = std::function<void()>;
    void setCompositePlugHooks(CompositePlugHook before, CompositePlugHook after);

    explicit HidMaestroAdapter(satellite::hidmaestro::IHidMaestroProvisioner& provisioner,
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
    void setRumbleCallback(RumbleCallback cb) override;
    void setLightbarCallback(LightbarCallback cb) override;
    void setTriggerEffectsCallback(TriggerEffectsCallback cb) override;
    void setPlayerLedsCallback(PlayerLedsCallback cb) override;
    void setSpeakerAudioCallback(SpeakerAudioCallback cb) override;
    void setMicLedCallback(MicLedCallback cb) override;
    bool submitMicAudioPcm(uint32_t serial, const int16_t* mono48k, size_t samples) override;

    // True once this serial's persona actually presented the endpoint, i.e.
    // the composite materialized and the helper handed over the ring. Exposed
    // for tests and diagnostics; the wire caps come from the catalog.
    bool hasSpeakerEndpoint(uint32_t serial) const;
    bool hasMicEndpoint(uint32_t serial) const;

    bool submitMotion(uint32_t serial, const MotionReport& report) override;
    bool submitBattery(uint32_t serial, const BatteryReport& report) override;
    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override;
    bool submitRelativeMouse(int dx, int dy, const MouseButtons& buttons, int wheelV) override;
    bool supportsRelativeMouse() const override { return true; }
    bool supportsMotionForType(uint8_t controllerType) const override;

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
        // speakerView is read-only (the helper produces), micView is written.
        const uint8_t* speakerView = nullptr;
        uint8_t* micView = nullptr;
        HANDLE speakerSection = nullptr;
        HANDLE speakerEvent = nullptr;
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
        GamepadReport switchPad{};
        MotionReport switchMotion{};
        bool fingerDown0 = false;
        bool fingerDown1 = false;
        std::chrono::steady_clock::time_point lastSonySubmit{};

        uint8_t payload[satellite::hidmaestro::INPUT_DATA_CAPACITY]{};
        uint8_t gip[satellite::hidmaestro::INPUT_GIP_LENGTH]{};

        // Atomic so isDevicePlugged can read without data races; written under
        // busMtx_ (plugin/unplug).
        std::atomic<bool> plugged{false};
    };

    satellite::hidmaestro::IHidMaestroProvisioner& provisioner_;
    AudioEnabledFn audioEnabled_;
    CompositePlugHook compositePlugBefore_;
    CompositePlugHook compositePlugAfter_;
    mutable std::mutex busMtx_;
    bool busOpen_ = false;

    std::array<IoSlot, MAX_BACKEND_CONTROLLERS + 1> io_;

    // Plug/unplug-time only (not hot path), so a map is fine.
    std::unordered_map<uint32_t, OutputWorker> outputWorkers_;
    std::unordered_map<uint32_t, OutputWorker> audioWorkers_;

    RumbleCallback rumbleCb_;
    LightbarCallback lightbarCb_;
    TriggerEffectsCallback triggerEffectsCb_;
    PlayerLedsCallback playerLedsCb_;
    SpeakerAudioCallback speakerAudioCb_;
    MicLedCallback micLedCb_;

    RelMouseButtonState relMouseBtns_;

    void startOutputWorker(uint32_t serial); // caller holds busMtx_
    void stopOutputWorker(uint32_t serial);  // caller holds busMtx_
    void outputLoop(uint32_t serial, HANDLE cancel, uint32_t lastSeq);
    void startAudioWorker(uint32_t serial); // caller holds busMtx_
    void stopAudioWorker(uint32_t serial);  // caller holds busMtx_
    void audioLoop(uint32_t serial, HANDLE cancel, uint32_t lastSeq);
    void releaseSlotLocked(IoSlot& slot);
    // Map the audio rings this plug came back with. Caller holds busMtx_;
    // failure is not fatal, it just leaves the pad without audio.
    void attachAudioLocked(IoSlot& slot, const satellite::hidmaestro::ProvisionResult& r);

    // Advance the Sony free-running clocks from wall time, pack the slot's
    // merged state for its identity, and publish the frame. Caller holds
    // busMtx_; the slot must be plugged.
    bool packAndWriteLocked(IoSlot& slot);
};
