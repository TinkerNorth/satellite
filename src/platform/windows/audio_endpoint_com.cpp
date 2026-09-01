// SPDX-License-Identifier: LGPL-3.0-or-later

#include "audio_endpoint_com.h"

#include "core/types.h"

#include <mmdeviceapi.h>
#include <propsys.h>
#include <cfgmgr32.h>

#include <chrono>
#include <vector>

extern void logMsg(LogLevel level, const std::string& source, const std::string& message);

namespace satellite {
namespace audioguard {

namespace {

const char* const LOG_SOURCE = "audio-guard";

// Declared locally rather than taken from the SDK's __declspec(uuid) classes:
// the IPolicyConfig pair is undocumented and has no header anywhere, and doing
// all four the same way keeps this file free of initguid.h/uuid.lib ordering.
const CLSID kMMDeviceEnumeratorClsid = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kMMDeviceEnumeratorIid = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const CLSID kPolicyConfigClientClsid = {
    0x870AF99C, 0x171D, 0x4F9E, {0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9}};
const IID kPolicyConfigIid = {
    0xF8679F50, 0x850A, 0x41CF, {0x9C, 0x72, 0x43, 0x0F, 0x29, 0x02, 0x90, 0xC8}};

// No public header names this one. It carries the endpoint's KS filter path,
// which is the only property tying an MMDevice back to a PnP device instance.
const PROPERTYKEY kPkeyDeviceKsFilterPath = {
    {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2};

const DEVPROPKEY kDevpkeyDeviceParent = {
    {0x4340a6c5, 0x93fa, 0x4706, {0x97, 0x2c, 0x7b, 0x64, 0x80, 0x08, 0xa5, 0xa7}}, 8};
const DEVPROPKEY kDevpkeyDeviceService = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 6};
const DEVPROPKEY kDevpkeyDeviceHardwareIds = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 3};

using GetProcessingPeriodProc = HRESULT(STDMETHODCALLTYPE*)(void*, LPCWSTR, INT, INT64*, INT64*);
using SetDefaultEndpointProc = HRESULT(STDMETHODCALLTYPE*)(void*, LPCWSTR, INT);

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string toUtf8(const wchar_t* s, size_t len) {
    if (!s || len == 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len), out.data(), n, nullptr, nullptr);
    return out;
}

std::string toUtf8(const wchar_t* s) { return s ? toUtf8(s, wcslen(s)) : std::string(); }

std::string hrText(HRESULT hr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

IMMDeviceEnumerator* createEnumerator() {
    IMMDeviceEnumerator* e = nullptr;
    HRESULT hr = CoCreateInstance(kMMDeviceEnumeratorClsid, nullptr, CLSCTX_INPROC_SERVER,
                                  kMMDeviceEnumeratorIid, reinterpret_cast<void**>(&e));
    if (FAILED(hr)) return nullptr;
    return e;
}

bool readDefaultId(IMMDeviceEnumerator* enumerator, Role role, std::string& outId) {
    outId.clear();
    if (!enumerator) return false;
    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(
        eRender, static_cast<ERole>(static_cast<int>(role)), &device);
    if (FAILED(hr) || !device) return false;

    LPWSTR id = nullptr;
    hr = device->GetId(&id);
    if (SUCCEEDED(hr) && id) outId = toUtf8(id);
    if (id) CoTaskMemFree(id);
    device->Release();
    return !outId.empty();
}

bool readKsFilterPath(IMMDeviceEnumerator* enumerator, const std::string& endpointId,
                      std::string& outPath) {
    outPath.clear();
    if (!enumerator || endpointId.empty()) return false;

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDevice(toWide(endpointId).c_str(), &device);
    if (FAILED(hr) || !device) return false;

    IPropertyStore* store = nullptr;
    hr = device->OpenPropertyStore(STGM_READ, &store);
    if (SUCCEEDED(hr) && store) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(kPkeyDeviceKsFilterPath, &pv)) && pv.vt == VT_LPWSTR) {
            outPath = toUtf8(pv.pwszVal);
        }
        PropVariantClear(&pv);
        store->Release();
    }
    device->Release();
    return !outPath.empty();
}

bool devNodeProperty(DEVINST devinst, const DEVPROPKEY& key, DEVPROPTYPE expected,
                     std::vector<BYTE>& buffer) {
    buffer.clear();
    DEVPROPTYPE type = 0;
    ULONG size = 0;
    CONFIGRET cr = CM_Get_DevNode_PropertyW(devinst, &key, &type, nullptr, &size, 0);
    if (cr != CR_BUFFER_SMALL || size == 0 || type != expected) return false;
    buffer.resize(size);
    cr = CM_Get_DevNode_PropertyW(devinst, &key, &type, buffer.data(), &size, 0);
    if (cr != CR_SUCCESS || type != expected) {
        buffer.clear();
        return false;
    }
    buffer.resize(size);
    return true;
}

std::wstring devNodeString(DEVINST devinst, const DEVPROPKEY& key) {
    std::vector<BYTE> buffer;
    if (!devNodeProperty(devinst, key, DEVPROP_TYPE_STRING, buffer)) return {};
    const size_t chars = buffer.size() / sizeof(wchar_t);
    if (chars == 0) return {};
    const wchar_t* raw = reinterpret_cast<const wchar_t*>(buffer.data());
    return std::wstring(raw, wcsnlen(raw, chars));
}

std::vector<std::string> devNodeStringList(DEVINST devinst, const DEVPROPKEY& key) {
    std::vector<std::string> out;
    std::vector<BYTE> buffer;
    if (!devNodeProperty(devinst, key, DEVPROP_TYPE_STRING_LIST, buffer)) return out;

    const size_t chars = buffer.size() / sizeof(wchar_t);
    const wchar_t* raw = reinterpret_cast<const wchar_t*>(buffer.data());
    size_t i = 0;
    while (i < chars && raw[i] != L'\0') {
        const size_t len = wcsnlen(raw + i, chars - i);
        out.push_back(toUtf8(raw + i, len));
        i += len;
        if (i < chars) ++i;
    }
    return out;
}

ChainResult walkParentChain(const std::string& instanceId, size_t maxHops) {
    ChainScanner scanner(maxHops);
    std::wstring current = toWide(instanceId);
    for (size_t hop = 0; hop <= maxHops; ++hop) {
        if (current.empty()) break;
        DEVINST devinst = 0;
        if (CM_Locate_DevNodeW(&devinst, const_cast<DEVINSTID_W>(current.c_str()),
                               CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            break;
        }
        DevNode node;
        node.instanceId = toUtf8(current.c_str(), current.size());
        node.service = toUtf8(devNodeString(devinst, kDevpkeyDeviceService).c_str());
        node.hardwareIds = devNodeStringList(devinst, kDevpkeyDeviceHardwareIds);
        if (!scanner.accept(node)) break;
        current = devNodeString(devinst, kDevpkeyDeviceParent);
    }
    return scanner.result();
}

ChainResult classifyEndpoint(IMMDeviceEnumerator* enumerator, const std::string& endpointId) {
    std::string ksPath;
    if (!readKsFilterPath(enumerator, endpointId, ksPath)) return ChainResult{};
    const std::string instanceId = stripKsFilterPrefix(ksPath);
    if (instanceId.empty()) return ChainResult{};
    return walkParentChain(instanceId, MAX_PARENT_HOPS);
}

// Hands back a validated object or nothing. `probeId` must name a LIVE
// endpoint: the two coclasses are told apart by their answer to slot 7, and a
// bogus id makes both fail identically.
IUnknown* bindValidatedPolicyConfig(const std::string& probeId, ProbeVerdict& verdict) {
    IUnknown* policy = nullptr;
    PolicyConfigProbe probe;
    HRESULT hr = CoCreateInstance(kPolicyConfigClientClsid, nullptr, CLSCTX_INPROC_SERVER,
                                  kPolicyConfigIid, reinterpret_cast<void**>(&policy));
    if (FAILED(hr) || !policy) {
        verdict = evaluatePolicyConfigProbe(probe);
        return nullptr;
    }
    probe.coCreateOk = true;

    void** vtable = *reinterpret_cast<void***>(policy);
    auto getProcessingPeriod =
        reinterpret_cast<GetProcessingPeriodProc>(vtable[POLICY_CONFIG_SLOT_GET_PROCESSING_PERIOD]);
    const std::wstring probeIdW = toWide(probeId);
    INT64 defaultPeriod = 0;
    INT64 minPeriod = 0;
    probe.probeHr = static_cast<int32_t>(
        getProcessingPeriod(policy, probeIdW.c_str(), TRUE, &defaultPeriod, &minPeriod));
    probe.defaultPeriod = defaultPeriod;
    probe.minPeriod = minPeriod;

    verdict = evaluatePolicyConfigProbe(probe);
    if (verdict != ProbeVerdict::Usable) {
        policy->Release();
        return nullptr;
    }
    return policy;
}

bool setDefaultEndpoint(const std::string& targetId, Role role, const std::string& probeId,
                        ProbeVerdict& verdict, HRESULT& setHr) {
    setHr = E_FAIL;
    IUnknown* policy = bindValidatedPolicyConfig(probeId, verdict);
    if (!policy) return false;

    void** vtable = *reinterpret_cast<void***>(policy);
    auto setDefault =
        reinterpret_cast<SetDefaultEndpointProc>(vtable[POLICY_CONFIG_SLOT_SET_DEFAULT_ENDPOINT]);
    setHr = setDefault(policy, toWide(targetId).c_str(), static_cast<INT>(role));
    policy->Release();
    return SUCCEEDED(setHr);
}

} // namespace

ComApartment::ComApartment() {
    // MULTITHREADED because the guard runs on a plug worker with no message
    // pump. AudioSes.dll is ThreadingModel=Both, so a caller thread already in
    // an STA gets RPC_E_CHANGED_MODE here and still works in its own apartment.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        usable_ = true;
        owned_ = false;
    } else if (SUCCEEDED(hr)) {
        usable_ = true;
        owned_ = true; // S_FALSE still owes a CoUninitialize
    }
}

ComApartment::~ComApartment() {
    if (owned_) CoUninitialize();
}

bool readDefaultRenderEndpointId(Role role, std::string& outId) {
    outId.clear();
    ComApartment com;
    if (!com.usable()) return false;
    IMMDeviceEnumerator* enumerator = createEnumerator();
    if (!enumerator) return false;
    const bool ok = readDefaultId(enumerator, role, outId);
    enumerator->Release();
    return ok;
}

bool endpointKsFilterPath(const std::string& endpointId, std::string& outPath) {
    outPath.clear();
    ComApartment com;
    if (!com.usable()) return false;
    IMMDeviceEnumerator* enumerator = createEnumerator();
    if (!enumerator) return false;
    const bool ok = readKsFilterPath(enumerator, endpointId, outPath);
    enumerator->Release();
    return ok;
}

ProbeVerdict probePolicyConfig(const std::string& probeEndpointId) {
    ComApartment com;
    if (!com.usable()) return ProbeVerdict::CoCreateFailed;
    ProbeVerdict verdict = ProbeVerdict::CoCreateFailed;
    IUnknown* policy = bindValidatedPolicyConfig(probeEndpointId, verdict);
    if (policy) policy->Release();
    return verdict;
}

bool endpointIsOurs(const std::string& endpointId, ChainResult* detail) {
    ComApartment com;
    if (!com.usable()) return false;
    IMMDeviceEnumerator* enumerator = createEnumerator();
    if (!enumerator) return false;
    const ChainResult result = classifyEndpoint(enumerator, endpointId);
    enumerator->Release();
    if (detail) *detail = result;
    return result.isOurs();
}

void AudioDefaultGuard::disableLocked(const std::string& why) {
    if (!enabled_) return;
    enabled_ = false;
    note_ = "disabled: " + why;
    logMsg(LogLevel::WARN, LOG_SOURCE,
           "Default-audio-endpoint guard disabled for this run (" + why + ")");
}

bool AudioDefaultGuard::endpointIsOursLocked(void* enumerator, const std::string& endpointId) {
    const std::string key = normalizeDeviceId(endpointId);
    if (key.empty()) return false;
    auto it = oursCache_.find(key);
    if (it != oursCache_.end()) return it->second;

    const ChainResult result =
        classifyEndpoint(static_cast<IMMDeviceEnumerator*>(enumerator), endpointId);
    oursCache_[key] = result.isOurs();
    return result.isOurs();
}

void AudioDefaultGuard::snapshotBeforePlug() {
    std::lock_guard<std::mutex> lk(mtx_);
    snapshot_.clear();
    oursCache_.clear();
    if (!enabled_) return;

    ComApartment com;
    if (!com.usable()) {
        disableLocked("CoInitializeEx failed");
        return;
    }
    IMMDeviceEnumerator* enumerator = createEnumerator();
    if (!enumerator) {
        disableLocked("MMDeviceEnumerator unavailable");
        return;
    }

    int captured = 0;
    for (Role role : ALL_ROLES) {
        std::string id;
        if (!readDefaultId(enumerator, role, id)) continue;
        RoleSnapshot& s = snapshot_.role(role);
        s.captured = true;
        s.priorId = id;
        s.priorWasOurs = endpointIsOursLocked(enumerator, id);
        ++captured;
    }
    enumerator->Release();
    note_ = "snapshot: " + std::to_string(captured) + "/" + std::to_string(ROLE_COUNT) + " roles";
}

bool AudioDefaultGuard::restoreRoleLocked(void* enumerator, Role role) {
    RoleSnapshot& s = snapshot_.role(role);

    RoleObservation obs;
    obs.haveCurrent =
        readDefaultId(static_cast<IMMDeviceEnumerator*>(enumerator), role, obs.currentId);
    if (obs.haveCurrent) obs.currentIsOurs = endpointIsOursLocked(enumerator, obs.currentId);

    const DefaultEndpointDecision decision = decideRestore(s, obs);
    if (decision.action != RestoreAction::Restore) return false;

    ProbeVerdict verdict = ProbeVerdict::CoCreateFailed;
    HRESULT setHr = E_FAIL;
    const bool ok = setDefaultEndpoint(decision.targetId, role, obs.currentId, verdict, setHr);
    noteRestoreResult(s, ok);

    if (ok) {
        ++restoredTotal_;
        logMsg(LogLevel::INFO, LOG_SOURCE,
               std::string("Virtual pad took the default render endpoint (") + roleName(role) +
                   "); restored " + decision.targetId);
        return true;
    }
    ++failedTotal_;
    if (verdict != ProbeVerdict::Usable) {
        disableLocked(std::string("IPolicyConfig validation: ") + probeVerdictName(verdict));
    } else {
        note_ = std::string("SetDefaultEndpoint ") + hrText(setHr);
        logMsg(LogLevel::WARN, LOG_SOURCE,
               std::string("Could not restore the default render endpoint (") + roleName(role) +
                   "): " + hrText(setHr));
    }
    return false;
}

RestorePass AudioDefaultGuard::restoreIfStolen() {
    std::lock_guard<std::mutex> lk(mtx_);
    RestorePass pass;
    if (!enabled_ || !anyRoleCouldRestoreLater(snapshot_)) return pass;

    ComApartment com;
    if (!com.usable()) {
        disableLocked("CoInitializeEx failed");
        return pass;
    }
    IMMDeviceEnumerator* enumerator = createEnumerator();
    if (!enumerator) {
        disableLocked("MMDeviceEnumerator unavailable");
        return pass;
    }

    const int failedBefore = failedTotal_;
    for (Role role : ALL_ROLES) {
        if (!enabled_) break;
        if (!couldRestoreLater(snapshot_.role(role))) continue;
        if (restoreRoleLocked(enumerator, role)) ++pass.restored;
    }
    enumerator->Release();

    pass.failed = failedTotal_ - failedBefore;
    pass.keepPolling = enabled_ && anyRoleCouldRestoreLater(snapshot_);
    return pass;
}

void AudioDefaultGuard::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    snapshot_.clear();
    oursCache_.clear();
}

bool AudioDefaultGuard::active() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return enabled_;
}

std::string AudioDefaultGuard::report() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string out = enabled_ ? "active" : "disabled";
    out += ", restored " + std::to_string(restoredTotal_);
    out += ", failed " + std::to_string(failedTotal_);
    if (!note_.empty()) out += " (" + note_ + ")";
    return out;
}

// Promotion is not synchronous with the plug returning: usbip attach, usbccgp,
// usbaudio.sys, the KS interface, AudioEndpointBuilder, then the heuristic
// re-runs. Five seconds of 250 ms polls covers that with room to spare, and the
// loop stops early the moment every role has settled.
namespace {
const int GUARD_POLL_INTERVAL_MS = 250;
const int GUARD_POLL_BUDGET = 20;
} // namespace

PlugGuardRunner::PlugGuardRunner(EnabledFn enabled) : enabled_(std::move(enabled)) {
    thread_ = std::thread([this] { worker(); });
}

PlugGuardRunner::~PlugGuardRunner() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PlugGuardRunner::beforeCompositePlug() {
    if (enabled_ && !enabled_()) return;
    guard_.reset();
    guard_.snapshotBeforePlug();
}

void PlugGuardRunner::afterCompositePlug() {
    if (enabled_ && !enabled_()) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        armed_ = true;
    }
    cv_.notify_one();
}

void PlugGuardRunner::worker() {
    std::unique_lock<std::mutex> lk(mtx_);
    while (true) {
        cv_.wait(lk, [this] { return armed_ || stop_; });
        if (stop_) return;
        armed_ = false;
        lk.unlock();

        for (int i = 0; i < GUARD_POLL_BUDGET; i++) {
            const RestorePass pass = guard_.restoreIfStolen();
            if (!pass.keepPolling) break;
            std::unique_lock<std::mutex> wait(mtx_);
            // A new plug re-arms mid-window: abandon this pass rather than
            // restoring against a snapshot that is about to be replaced.
            if (cv_.wait_for(wait, std::chrono::milliseconds(GUARD_POLL_INTERVAL_MS),
                             [this] { return armed_ || stop_; })) {
                break;
            }
        }

        lk.lock();
        if (stop_) return;
    }
}

} // namespace audioguard
} // namespace satellite
