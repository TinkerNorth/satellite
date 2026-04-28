// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * gamepad_adapter.h — IGamepadPort stub for macOS.
 *
 * Reports the bus as unavailable and refuses plug-in / submit operations.
 * Lets the SessionService still run end-to-end (pairing, discovery, auth,
 * telemetry), while clients learn via ACK_ERR_BACKEND_UNAVAIL that no
 * virtual controllers can be attached on this host.
 */
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
    void unplugDevice(uint32_t) override {}
    bool submitReport(uint32_t, const GamepadReport&) override { return false; }
    bool submitDS4Report(uint32_t, const GamepadReport&) override { return false; }
};
