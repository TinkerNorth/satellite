/*
 * adapters/vigem_adapter.h — IViGemPort implementation wrapping ViGEm driver IOCTLs.
 *
 * Owns: bus handle, per-serial submitEvent handles.
 */
#pragma once

#include "../core/ports.h"

#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include "ViGEm/BusShared.h"

#include <unordered_map>
#include <mutex>

class ViGEmAdapter : public IViGemPort {
  public:
    ViGEmAdapter();
    ~ViGEmAdapter() override;

    bool ensureBusOpen() override;
    void closeBus() override;
    bool isBusOpen() const override;
    bool pluginDevice(uint32_t serial) override;
    void unplugDevice(uint32_t serial) override;
    bool submitReport(uint32_t serial, const GamepadReport& report) override;
    bool isDriverInstalled() override;

  private:
    HANDLE busHandle_ = INVALID_HANDLE_VALUE;
    mutable std::mutex busMtx_;

    // Per-serial pre-allocated overlapped events for fast submission
    std::unordered_map<uint32_t, HANDLE> submitEvents_;
};
