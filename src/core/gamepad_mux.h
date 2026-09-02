// SPDX-License-Identifier: LGPL-3.0-or-later

// Composite IGamepadPort routing one shared serial space across several
// concrete backends (preference-ordered, e.g. ViGEm then HIDMaestro on
// Windows). SessionService keeps its single port reference and single serial
// pool; the mux records which child owns each serial at plug time and routes
// every per-serial call there. Pure: only core headers, so it unit-tests with
// mock ports on every platform.
#pragma once

#include "core/ports.h"

#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace satellite {

class GamepadMux : public IGamepadPort {
  public:
    // `ports` in preference order; non-owning, must outlive the mux.
    explicit GamepadMux(std::vector<IGamepadPort*> ports) : ports_(std::move(ports)) {}

    bool ensureBusOpen() override {
        bool any = false;
        for (IGamepadPort* p : ports_) any = p->ensureBusOpen() || any;
        return any;
    }

    void closeBus() override {
        for (IGamepadPort* p : ports_) p->closeBus();
    }

    bool isBusOpen() const override {
        for (IGamepadPort* p : ports_) {
            if (p->isBusOpen()) return true;
        }
        return false;
    }

    // Static union: the catalog offers a type when any configured backend can
    // materialize it; a missing driver surfaces at plug time, not as
    // invalidType.
    bool supportsIdentity(GamepadIdentity identity) const override {
        for (IGamepadPort* p : ports_) {
            if (p->supportsIdentity(identity)) return true;
        }
        return false;
    }

    // First supporting child that accepts the plug wins; a child whose driver
    // is absent or bus is down refuses fast and the next one is tried.
    bool pluginDevice(uint32_t serial, GamepadIdentity identity) override {
        return pluginDevicePreferring(serial, identity, std::string());
    }

    bool pluginDevicePreferring(uint32_t serial, GamepadIdentity identity,
                                const std::string& preferredBackend) override {
        if (!validSerial(serial)) return false;
        IGamepadPort* tried = nullptr;
        if (!preferredBackend.empty()) {
            for (IGamepadPort* p : ports_) {
                if (preferredBackend != p->backendId()) continue;
                tried = p;
                if (p->supportsIdentity(identity) && p->pluginDevice(serial, identity)) {
                    owner(serial).store(p, std::memory_order_release);
                    return true;
                }
                break;
            }
        }
        for (IGamepadPort* p : ports_) {
            if (p == tried) continue;
            if (!p->supportsIdentity(identity)) continue;
            if (p->pluginDevice(serial, identity)) {
                owner(serial).store(p, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    bool unplugDevice(uint32_t serial) override {
        IGamepadPort* p = ownerOf(serial);
        if (p == nullptr) return true;
        // False = removal unconfirmed: keep the owner so the quarantined
        // serial still routes to the child holding the zombie target.
        if (!p->unplugDevice(serial)) return false;
        owner(serial).store(nullptr, std::memory_order_release);
        return true;
    }

    bool isDevicePlugged(uint32_t serial) const override {
        IGamepadPort* p = ownerOf(serial);
        return p != nullptr && p->isDevicePlugged(serial);
    }

    bool submitReport(uint32_t serial, const GamepadReport& report) override {
        IGamepadPort* p = ownerOf(serial);
        return p != nullptr && p->submitReport(serial, report);
    }

    bool submitMotion(uint32_t serial, const MotionReport& report) override {
        IGamepadPort* p = ownerOf(serial);
        return p != nullptr && p->submitMotion(serial, report);
    }

    bool submitBattery(uint32_t serial, const BatteryReport& report) override {
        IGamepadPort* p = ownerOf(serial);
        return p != nullptr && p->submitBattery(serial, report);
    }

    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override {
        IGamepadPort* p = ownerOf(serial);
        return p != nullptr && p->submitTouchpad(serial, report);
    }

    bool submitRelativeMouse(int dx, int dy, const MouseButtons& buttons, int wheelV) override {
        for (IGamepadPort* p : ports_) {
            if (p->supportsRelativeMouse()) return p->submitRelativeMouse(dx, dy, buttons, wheelV);
        }
        return false;
    }

    bool supportsRelativeMouse() const override {
        for (IGamepadPort* p : ports_) {
            if (p->supportsRelativeMouse()) return true;
        }
        return false;
    }

    // Answered by the child the plug router would prefer for this type, which
    // is the one that materializes it in the common case.
    bool supportsMotionForType(uint8_t controllerType) const override {
        const GamepadIdentity id = controllerIdentity(controllerType);
        for (IGamepadPort* p : ports_) {
            if (p->supportsIdentity(id)) return p->supportsMotionForType(controllerType);
        }
        return false;
    }

    bool motionBackendOk(uint32_t serial) const override {
        IGamepadPort* p = ownerOf(serial);
        return p == nullptr || p->motionBackendOk(serial);
    }

    void setRumbleCallback(RumbleCallback cb) override {
        for (IGamepadPort* p : ports_) p->setRumbleCallback(cb);
    }

    void setLightbarCallback(LightbarCallback cb) override {
        for (IGamepadPort* p : ports_) p->setLightbarCallback(cb);
    }

    void setTriggerEffectsCallback(TriggerEffectsCallback cb) override {
        for (IGamepadPort* p : ports_) p->setTriggerEffectsCallback(cb);
    }

    void setPlayerLedsCallback(PlayerLedsCallback cb) override {
        for (IGamepadPort* p : ports_) p->setPlayerLedsCallback(cb);
    }

    void setSpeakerAudioCallback(SpeakerAudioCallback cb) override {
        for (IGamepadPort* p : ports_) p->setSpeakerAudioCallback(cb);
    }

    void setMicLedCallback(MicLedCallback cb) override {
        for (IGamepadPort* p : ports_) p->setMicLedCallback(cb);
    }

    bool submitMicAudioPcm(uint32_t serial, const int16_t* mono48k, size_t samples) override {
        IGamepadPort* p = ownerOf(serial);
        return p != nullptr && p->submitMicAudioPcm(serial, mono48k, samples);
    }

    const char* backendIdForSerial(uint32_t serial) const override {
        const IGamepadPort* p = ownerOf(serial);
        return p == nullptr ? "" : p->backendId();
    }

    // Which child owns a serial right now (nullptr = unplugged). For status
    // introspection and tests; per-serial routing uses it internally.
    IGamepadPort* ownerOf(uint32_t serial) const {
        if (!validSerial(serial)) return nullptr;
        return owner(serial).load(std::memory_order_acquire);
    }

  private:
    static bool validSerial(uint32_t serial) {
        return serial >= 1 && serial <= MAX_BACKEND_CONTROLLERS;
    }

    std::atomic<IGamepadPort*>& owner(uint32_t serial) { return owners_[serial]; }
    const std::atomic<IGamepadPort*>& owner(uint32_t serial) const { return owners_[serial]; }

    std::vector<IGamepadPort*> ports_;
    // Slot 0 unused so indexing matches wire-level serial numbers. Atomics so
    // the lock-free submit path reads a coherent owner while plug/unplug (in
    // the session's lock) rebinds other serials.
    std::array<std::atomic<IGamepadPort*>, MAX_BACKEND_CONTROLLERS + 1> owners_{};
};

} // namespace satellite
