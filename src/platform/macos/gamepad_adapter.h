// SPDX-License-Identifier: LGPL-3.0-or-later
// IGamepadPort stub: macOS has no virtual gamepad bus, so the bus reports
// unavailable and plug-in/submit are refused. SessionService still runs
// end-to-end (pairing, discovery, auth, telemetry); clients learn no
// controllers can attach via the backendUnavailable apply result.
#pragma once

#include "core/ports.h"

class GamepadAdapter : public IGamepadPort {
  public:
    GamepadAdapter() = default;
    ~GamepadAdapter() override = default;

    bool ensureBusOpen() override { return false; }
    void closeBus() override {}
    bool isBusOpen() const override { return false; }
    bool pluginDevice(uint32_t) override { return false; }
    bool pluginDeviceDS4(uint32_t) override { return false; }
    bool unplugDevice(uint32_t) override { return true; } // nothing existed to remove
    bool isDevicePlugged(uint32_t) const override { return false; }
    bool submitReport(uint32_t, const GamepadReport&) override { return false; }
    bool submitDS4Report(uint32_t, const GamepadReport&) override { return false; }

    // Accepted for uniform composition across platforms but never invoked: no
    // virtual gamepads means no game ever produces rumble events.
    void setRumbleCallback(RumbleCallback) override {}
};
