// SPDX-License-Identifier: LGPL-3.0-or-later

// IHidMaestroProvisioner over the bundled elevated helper
// (satellite-hm-helper.exe). Satellite stays asInvoker: the helper carries the
// HIDMaestro SDK (driver install, SwDevice + Global\ section creation — all of
// which need an elevated token) and hands duplicated section/event handles
// back over a named pipe, one JSON line per request. Two ways to reach it, in
// order: the SatelliteHmBroker service setup registers (LocalSystem, demand-
// start, well-known pipe; no prompt), else a helper spawned `runas`, which
// shows one UAC prompt per session, deferred to the first HIDMaestro plug.
#pragma once

#include "hidmaestro_provisioner.h"
#include "core/tracked_mutex.h"
#include "core/types.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace satellite {
namespace hidmaestro {

inline constexpr DWORD HELPER_PIPE_WAIT_MS = 10000;
inline constexpr DWORD HELPER_PROVISION_WAIT_MS = 120000;
inline constexpr DWORD HELPER_LAUNCH_WAIT_MS = 120000;
inline constexpr DWORD HELPER_EXIT_WAIT_MS = 5000;
// How long a lost pad's unplug waits for its answer: one maintenance tick, on
// whose thread it runs. A helper answers it at once (helper Disposals); one
// that does not is read later, before the next request's answer, within that
// request's own bound.
inline constexpr DWORD HELPER_LOST_UNPLUG_WAIT_MS = 100;
// What one controller's removal costs at worst against a program that holds it
// open, from HIDMaestro's DeviceManager.cs (v2.1.0): its removal query waits
// about 5.4 s on a holder that never reads, Windows' own wait; then, once the
// driver has failed the device on request, up to 5 s for Windows' surprise
// removal to take it down (SurpriseRemovalWaitMs) and up to 2 s for a late
// notification (RemovedBoundMs). The SDK scales those two by
// HIDMAESTRO_TIMEOUT_SCALE. A driver that does not answer the request costs 2 s
// in their place.
inline constexpr DWORD HM_REFUSED_QUERY_MS = 5400;
inline constexpr DWORD HM_SURPRISE_REMOVAL_WAIT_MS = 5000;
inline constexpr DWORD HM_REMOVED_BOUND_MS = 2000;
// What the shutdown's answer itself gets once the last removal is done.
inline constexpr DWORD HELPER_SHUTDOWN_ANSWER_MS = 5000;
// The longest a shutdown waits at any timeout scale: nine minutes, so a
// Satellite's exit stays inside the ten minutes a relaunch waits for it
// (single_instance.h; app_lifecycle.cpp checks it). Reached just above a scale
// of 4.
inline constexpr DWORD HELPER_SHUTDOWN_WAIT_CAP_MS = 540000;

// How long a shutdown waits for its answer, which the helper gives once every
// controller it held is removed: 16 such removals one after another, as the SDK
// takes removals under its PnP lock, at the scale the helper's SDK resolved
// (never shortened, as the device watch's grace is not), then the answer. 203.4
// s at a scale of 1. A removal that runs into the SDK's own removal budgets is
// not covered (one software device with children can hold the PnP lock about
// 230 s, INTERNALS.md): satellite.exe then ends the helper, and the next
// launch's sweep removes what it left.
constexpr DWORD helperShutdownWaitMs(double timeoutScale) {
    const double scale = timeoutScale > 1.0 && timeoutScale <= 100.0 ? timeoutScale : 1.0;
    const double removal =
        HM_REFUSED_QUERY_MS + (HM_SURPRISE_REMOVAL_WAIT_MS + HM_REMOVED_BOUND_MS) * scale;
    const double wait = MAX_BACKEND_CONTROLLERS * removal + HELPER_SHUTDOWN_ANSWER_MS;
    return wait < HELPER_SHUTDOWN_WAIT_CAP_MS ? static_cast<DWORD>(wait)
                                              : HELPER_SHUTDOWN_WAIT_CAP_MS;
}
inline constexpr DWORD HELPER_SHUTDOWN_WAIT_MS = helperShutdownWaitMs(1.0);

enum class HelperRequest { Hello, Provision, Deprovision, Shutdown };

constexpr DWORD helperRequestWaitMs(HelperRequest request) {
    switch (request) {
    case HelperRequest::Provision:
        return HELPER_PROVISION_WAIT_MS;
    case HelperRequest::Shutdown:
        return HELPER_SHUTDOWN_WAIT_MS;
    case HelperRequest::Hello:
    case HelperRequest::Deprovision:
        return HELPER_PIPE_WAIT_MS;
    }
    return HELPER_PIPE_WAIT_MS;
}

// Split probes so the platform probeBackend() can distinguish DRIVER_MISSING
// from HELPER_MISSING without constructing a client.
std::wstring helperBinaryPath(); // beside satellite.exe
bool helperBinaryPresent();
// Driver footprint: HKLM\SOFTWARE\HIDMaestro (written after a successful
// deploy) or the hidmaestro.inf_* driver-store directories. Non-admin.
bool driverInstalled();
std::string installedDriverVersion();
bool brokerServiceRegistered();
bool installDriver(std::string& outError);

struct HelperChannel;
class HelperClient;

void connectBrokerWhenDriverInstalled(HelperClient* client);

class HelperClient : public IHidMaestroProvisioner {
  public:
    HelperClient();
    ~HelperClient() override;

    bool installed() const override;
    bool isReady() const override;
    bool ensureReady() override;
    void shutdown() override;
    bool provision(uint32_t serial, GamepadIdentity identity, bool audio,
                   ProvisionResult& out) override;
    bool deprovision(uint32_t serial, Teardown why) override;
    std::vector<uint32_t> takeSettledSerials() override;
    uint64_t liveChannel() const override;

    void connectBrokerInBackground();

#ifdef SATELLITE_BUILD_TESTS
    struct TestTransports {
        std::function<HANDLE()> broker;
        std::function<HANDLE(const std::wstring& pipeName)> launch;
    };
    bool adoptChannelForTest(HANDLE pipe);
    void setTransportsForTest(TestTransports transports);
    void setPipeWaitForTest(DWORD ms);
    void setProvisionWaitForTest(DWORD ms);
    void setLaunchWaitForTest(DWORD ms);
    void setExitWaitForTest(DWORD ms);
    void setLostUnplugWaitForTest(DWORD ms);
    void setShutdownWaitForTest(DWORD ms);
    DWORD shutdownWaitForTest() const;
    bool waitForBackgroundConnectForTest(DWORD ms);
    static uint64_t pipeReadsForTest();
#endif

  private:
    bool startLocked();
    HANDLE openBroker();
    bool connectBroker(HelperChannel& channel);
    bool spawnHelper(HelperChannel& channel);
    HANDLE launch(const std::wstring& pipeName);
    DWORD waitFor(HelperRequest request) const;
    bool openChannel(HelperChannel& channel, HANDLE pipe, HANDLE process, HANDLE cancel);
    void connectBrokerAhead();
    void awaitBackgroundConnect();
    void adoptStagedLocked();
    void setChannelLocked(std::unique_ptr<HelperChannel> channel);
    void stopLocked(bool sendShutdown);
    bool requestLocked(HelperRequest request, const std::string& line, std::string& response);
    bool deprovisionLocked(uint32_t serial, Teardown why);
    bool lostUnplugLocked(uint32_t serial, const std::string& line);
    bool drainOwedLocked(ULONGLONG deadline);
    void settleLocked(uint32_t serial);
    void settleSweptLocked();

    mutable TrackedMutex mtx_{"hidmaestro.helper"};
    std::unique_ptr<HelperChannel> channel_;
    std::atomic<bool> channelUp_{false};
    std::atomic<HelperChannel*> staged_{nullptr};
    std::atomic<bool> connecting_{false};
    std::atomic<DWORD> pipeWaitMs_{helperRequestWaitMs(HelperRequest::Hello)};
    std::atomic<DWORD> provisionWaitMs_{helperRequestWaitMs(HelperRequest::Provision)};
    std::atomic<DWORD> launchWaitMs_{HELPER_LAUNCH_WAIT_MS};
    std::atomic<DWORD> exitWaitMs_{HELPER_EXIT_WAIT_MS};
    std::atomic<DWORD> lostUnplugWaitMs_{HELPER_LOST_UNPLUG_WAIT_MS};
    // 0: the helper's own bound, at the timeout scale its plugs report.
    std::atomic<DWORD> shutdownWaitMs_{0};
    // mtx_: each channel adopted counts one up; a serial whose removal went
    // unconfirmed keeps the count it went on, and a later channel's first
    // plug, which sweeps what an earlier helper left, settles it.
    uint64_t generation_ = 0;
    std::map<uint32_t, uint64_t> unconfirmed_;
    std::vector<uint32_t> settled_;
    // mtx_: the channel each plugged serial was served on.
    std::map<uint32_t, uint64_t> pluggedOn_;
    // The current channel's count, 0 with none; written under mtx_, read
    // without it from the maintenance tick (liveChannel).
    std::atomic<uint64_t> liveGeneration_{0};
    HANDLE cancel_ = nullptr;
    HANDLE connectDone_ = nullptr;
    std::thread connectThread_;
#ifdef SATELLITE_BUILD_TESTS
    bool transportsActive_ = false;
    TestTransports transports_;
#endif
};

#ifdef SATELLITE_BUILD_TESTS
void notifyDriverInstalledForTest();
// Runs once, the next time a pipe wait runs out, just before its I/O is
// cancelled: where a test lands an answer so its read completes in that gap.
void setBeforeCancelForTest(std::function<void()> hook);
#endif

} // namespace hidmaestro
} // namespace satellite
