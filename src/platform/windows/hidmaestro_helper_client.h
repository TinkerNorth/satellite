// SPDX-License-Identifier: LGPL-3.0-or-later

// IHidMaestroProvisioner over the bundled elevated helper
// (satellite-hm-helper.exe). Satellite stays asInvoker: the helper carries the
// HIDMaestro SDK (driver install, SwDevice + Global\ section creation — all of
// which need an elevated token) and hands duplicated section/event handles
// back over a private named pipe, one JSON line per request. Spawning it
// shows one UAC prompt per session, deferred to the first HIDMaestro plug.
#pragma once

#include "hidmaestro_provisioner.h"

#include <winsock2.h>
#include <windows.h>

#include <mutex>
#include <string>

namespace satellite {
namespace hidmaestro {

// Split probes so the platform probeBackend() can distinguish DRIVER_MISSING
// from HELPER_MISSING without constructing a client.
std::wstring helperBinaryPath(); // beside satellite.exe
bool helperBinaryPresent();
// Driver footprint: HKLM\SOFTWARE\HIDMaestro (written after a successful
// deploy) or the hidmaestro.inf_* driver-store directories. Non-admin.
bool driverInstalled();

class HelperClient : public IHidMaestroProvisioner {
  public:
    HelperClient() = default;
    ~HelperClient() override;

    bool installed() const override;
    bool isReady() const override;
    bool ensureReady() override;
    void shutdown() override;
    bool provision(uint32_t serial, GamepadIdentity identity, bool audio,
                   ProvisionResult& out) override;
    bool deprovision(uint32_t serial) override;

  private:
    bool startLocked();
    void stopLocked(bool sendShutdown);
    bool requestLocked(const std::string& line, std::string& response);

    mutable std::mutex mtx_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE ioEvent_ = nullptr; // overlapped read/write completion, reused
    HANDLE helperProcess_ = nullptr;
};

} // namespace hidmaestro
} // namespace satellite
