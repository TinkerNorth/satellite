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
// that does not is read later, before the next request's answer.
inline constexpr DWORD HELPER_LOST_UNPLUG_WAIT_MS = 100;

enum class HelperRequest { Hello, Provision, Deprovision, Shutdown };

constexpr DWORD helperRequestWaitMs(HelperRequest request) {
    return request == HelperRequest::Provision ? HELPER_PROVISION_WAIT_MS : HELPER_PIPE_WAIT_MS;
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
    bool drainOwedLocked(DWORD waitMs);
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
    // mtx_: each channel adopted counts one up; a serial whose removal went
    // unconfirmed keeps the count it went on, and a later channel's first
    // plug, which sweeps what an earlier helper left, settles it.
    uint64_t generation_ = 0;
    std::map<uint32_t, uint64_t> unconfirmed_;
    std::vector<uint32_t> settled_;
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
#endif

} // namespace hidmaestro
} // namespace satellite
