// SPDX-License-Identifier: LGPL-3.0-or-later

// IO shell for the controller-audio default-endpoint guard: MMDevice reads, the
// cfgmgr32 parent walk, and the one undocumented write. Every rule lives in
// audio_default_guard.h; nothing here decides anything.
//
// Everything fails soft. A guard that cannot reach COM, cannot read a default,
// cannot identify an endpoint or cannot validate IPolicyConfig does nothing at
// all — this is a nicety, never a reason to fail a controller plug.
//
// Satellite stays asInvoker and this belongs here rather than in the elevated
// helper: SetDefaultEndpoint RPCs to the audio service, which does the
// privileged registry write itself, so the call succeeds unelevated.
#pragma once

#include "audio_default_guard.h"
#include "audio_guard_worker.h"
#include "core/tracked_mutex.h"

#include <winsock2.h>
#include <windows.h>

#include <functional>
#include <string>
#include <unordered_map>

namespace satellite {
namespace audioguard {

// Balanced CoInitializeEx/CoUninitialize. RPC_E_CHANGED_MODE means the thread
// is already in the other apartment and is usable as-is but must NOT be
// uninitialised here; S_FALSE still owes a CoUninitialize.
class ComApartment {
  public:
    ComApartment();
    ~ComApartment();
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    bool usable() const { return usable_; }

  private:
    bool usable_ = false;
    bool owned_ = false;
};

// Self-contained diagnostics: each initialises COM for the calling thread.
bool readDefaultRenderEndpointId(Role role, std::string& outId);
bool endpointKsFilterPath(const std::string& endpointId, std::string& outPath);
bool endpointIsOurs(const std::string& endpointId, ChainResult* detail = nullptr);

// The read-only half of the IPolicyConfig binding, with no write behind it.
// `probeEndpointId` must name a live endpoint, e.g. the current default.
ProbeVerdict probePolicyConfig(const std::string& probeEndpointId);

// Snapshot the default render endpoints before a composite plug, then call
// restoreIfStolen() repeatedly over a bounded window afterwards. Promotion is
// not synchronous with the plug returning (usbip attach, usbccgp, usbaudio.sys,
// the KS interface, AudioEndpointBuilder, then the heuristic re-runs), so a
// single immediate check misses it; the timing is the caller's to choose. Each
// pass restores whatever it finds stolen, and a second promotion inside the
// window (a second pad) is restored again; only the caller's budget ends it.
class AudioDefaultGuard : public GuardBackend {
  public:
    bool active() const;
    std::string report() const;

  private:
    void snapshotBeforePlug() override;
    RestorePass restoreIfStolen() override;
    bool restoreRole(void* enumerator, Role role);
    bool endpointIsOurs(void* enumerator, const std::string& endpointId);
    void disable(const std::string& why);
    void note(const std::string& text);
    int failedCount() const;

    mutable TrackedMutex mtx_{"audio.guard"};
    Snapshot snapshot_;
    std::unordered_map<std::string, bool> oursCache_;
    bool enabled_ = true;
    int restoredTotal_ = 0;
    int failedTotal_ = 0;
    std::string note_;
};

// Drives the guard around a composite plug. The polling lives on one
// long-lived worker rather than a thread per plug.
class PlugGuardRunner {
  public:
    using EnabledFn = std::function<bool()>;

    explicit PlugGuardRunner(EnabledFn enabled = {});
    PlugGuardRunner(const PlugGuardRunner&) = delete;
    PlugGuardRunner& operator=(const PlugGuardRunner&) = delete;

    void beforeCompositePlug();
    void afterCompositePlug();

    std::string report() const { return guard_.report(); }

  private:
    AudioDefaultGuard guard_;
    GuardWorker worker_;
};

} // namespace audioguard
} // namespace satellite
