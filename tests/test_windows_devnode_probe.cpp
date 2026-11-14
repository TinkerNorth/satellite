// SPDX-License-Identifier: LGPL-3.0-or-later

// Probes the Windows Plug and Play behaviour the HIDMaestro helper's device
// watch builds on, with two software devices (SwDeviceCreate) shaped like a
// HIDMaestro pad: a parent and a child below it. No HIDMaestro driver is
// involved. It needs an elevated process; it skips (77) when it cannot run,
// except on GitHub Actions, where skipping is a failure.
#include "test_util.h"

#include <winsock2.h>
#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cwctype>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int SKIPPED = 77;
constexpr auto SETTLE = std::chrono::milliseconds(3000);
constexpr auto STATE_BUDGET = std::chrono::milliseconds(30000);
constexpr auto POLL = std::chrono::milliseconds(25);
constexpr ULONG SW_CAPABILITIES_REMOVABLE = 0x1;
constexpr ULONG SW_CAPABILITIES_SILENT_INSTALL = 0x2;
constexpr ULONG SW_CAPABILITIES_NO_DISPLAY_IN_UI = 0x4;
constexpr DWORD LUA_TOKEN_FLAG = 0x4;
constexpr const wchar_t* ENUMERATOR = L"SatelliteDevnodeProbe";
constexpr const wchar_t* ROOT_PARENT = L"HTREE\\ROOT\\0";

// swdevice.h, which MinGW does not ship. Resolved at run time, as are the
// notification calls, so the binary loads where they are missing (Wine).
struct SwCreateInfo {
    ULONG cbSize;
    PCWSTR pszInstanceId;
    PCWSTR pszzHardwareIds;
    PCWSTR pszzCompatibleIds;
    const GUID* pContainerId;
    ULONG CapabilityFlags;
    PCWSTR pszDeviceDescription;
    PCWSTR pszDeviceLocation;
    const SECURITY_DESCRIPTOR* pSecurityDescriptor;
};
using SwHandle = HANDLE;
using SwCreateCallback = VOID(WINAPI*)(SwHandle, HRESULT, PVOID, PCWSTR);
using SwDeviceCreateFn = HRESULT(WINAPI*)(PCWSTR, PCWSTR, const SwCreateInfo*, ULONG, const void*,
                                          SwCreateCallback, PVOID, SwHandle*);
using SwDeviceCloseFn = void(WINAPI*)(SwHandle);
using RegisterFn = CONFIGRET(WINAPI*)(PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK,
                                      PHCMNOTIFICATION);
using UnregisterFn = CONFIGRET(WINAPI*)(HCMNOTIFICATION);

// GetProcAddress hands back FARPROC; GCC documents `void (*)()` as the one
// function type every function pointer converts through without a warning.
template <typename Fn> Fn procAddress(HMODULE module, const char* name) {
    using Anything = void (*)();
    return reinterpret_cast<Fn>(reinterpret_cast<Anything>(GetProcAddress(module, name)));
}

struct Api {
    SwDeviceCreateFn create = nullptr;
    SwDeviceCloseFn close = nullptr;
    RegisterFn registerNotification = nullptr;
    UnregisterFn unregisterNotification = nullptr;

    bool complete() const {
        return create != nullptr && close != nullptr && registerNotification != nullptr &&
               unregisterNotification != nullptr;
    }
};

Api resolveApi() {
    const HMODULE cfgmgr = LoadLibraryW(L"cfgmgr32.dll");
    if (cfgmgr == nullptr) return {};
    return {procAddress<SwDeviceCreateFn>(cfgmgr, "SwDeviceCreate"),
            procAddress<SwDeviceCloseFn>(cfgmgr, "SwDeviceClose"),
            procAddress<RegisterFn>(cfgmgr, "CM_Register_Notification"),
            procAddress<UnregisterFn>(cfgmgr, "CM_Unregister_Notification")};
}

bool underWine() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

bool onGitHubActions() {
    const char* value = std::getenv("GITHUB_ACTIONS");
    return value != nullptr && std::string(value) == "true";
}

bool tokenElevated(HANDLE token) {
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL read =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    return read != 0 && elevation.TokenIsElevated != 0;
}

bool processElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    const bool elevated = tokenElevated(token);
    CloseHandle(token);
    return elevated;
}

std::string narrow(const std::wstring& wide) {
    std::string out;
    for (const wchar_t c : wide) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

bool sameId(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towupper(a[i]) != std::towupper(b[i])) return false;
    }
    return true;
}

const char* actionName(CM_NOTIFY_ACTION action) {
    switch (action) {
    case CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL:
        return "INTERFACEARRIVAL";
    case CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL:
        return "INTERFACEREMOVAL";
    case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
        return "QUERYREMOVE";
    case CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED:
        return "QUERYREMOVEFAILED";
    case CM_NOTIFY_ACTION_DEVICEREMOVEPENDING:
        return "REMOVEPENDING";
    case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE:
        return "REMOVECOMPLETE";
    case CM_NOTIFY_ACTION_DEVICECUSTOMEVENT:
        return "CUSTOMEVENT";
    case CM_NOTIFY_ACTION_DEVICEINSTANCEENUMERATED:
        return "ENUMERATED";
    case CM_NOTIFY_ACTION_DEVICEINSTANCESTARTED:
        return "STARTED";
    case CM_NOTIFY_ACTION_DEVICEINSTANCEREMOVED:
        return "REMOVED";
    case CM_NOTIFY_ACTION_MAX:
        return "MAX";
    }
    return "?";
}

struct Event {
    int registration;
    CM_NOTIFY_ACTION action;
    std::wstring instanceId;
    DWORD thread;
};

// Every callback of every registration lands here, with the thread it ran on.
class Recorder {
  public:
    void record(int registration, CM_NOTIFY_ACTION action, const std::wstring& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        events_.push_back({registration, action, id, GetCurrentThreadId()});
    }

    size_t mark() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return events_.size();
    }

    std::vector<Event> since(size_t mark) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return std::vector<Event>(events_.begin() + static_cast<std::ptrdiff_t>(mark),
                                  events_.end());
    }

  private:
    mutable std::mutex mtx_;
    std::vector<Event> events_;
};

struct Registration {
    int which;
    Recorder* recorder;
    HCMNOTIFICATION handle = nullptr;
    CONFIGRET result = CR_FAILURE;
};

DWORD CALLBACK onNotification(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                              PCM_NOTIFY_EVENT_DATA data, DWORD) {
    auto* registration = static_cast<Registration*>(context);
    const bool forInstance =
        data != nullptr && data->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE;
    const std::wstring id = forInstance ? std::wstring(data->u.DeviceInstance.InstanceId) : L"";
    registration->recorder->record(registration->which, action, id);
    return ERROR_SUCCESS;
}

CM_NOTIFY_FILTER instanceFilter(const std::wstring& instanceId) {
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE;
    const size_t length = std::min(instanceId.size(), static_cast<size_t>(MAX_DEVICE_ID_LEN - 1));
    std::copy_n(instanceId.c_str(), length, filter.u.DeviceInstance.InstanceId);
    return filter;
}

CM_NOTIFY_FILTER allInstancesFilter() {
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_DEVICE_INSTANCES;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE;
    return filter;
}

void registerFor(const Api& api, Registration& registration, CM_NOTIFY_FILTER filter) {
    registration.result =
        api.registerNotification(&filter, &registration, onNotification, &registration.handle);
}

void unregister(const Api& api, Registration& registration) {
    if (registration.handle != nullptr) api.unregisterNotification(registration.handle);
    registration.handle = nullptr;
}

// One software device, created and waited for.
struct SoftDevice {
    SwHandle handle = nullptr;
    std::wstring instanceId;
    HRESULT result = E_PENDING;
    std::mutex mtx;
    std::condition_variable done;
    bool finished = false;
};

VOID WINAPI onCreated(SwHandle, HRESULT result, PVOID context, PCWSTR instanceId) {
    auto* device = static_cast<SoftDevice*>(context);
    std::lock_guard<std::mutex> lk(device->mtx);
    device->result = result;
    device->instanceId = instanceId != nullptr ? instanceId : L"";
    device->finished = true;
    device->done.notify_all();
}

bool createDevice(const Api& api, SoftDevice& device, const std::wstring& parent,
                  const std::wstring& instance) {
    const std::wstring hardwareIds = std::wstring(L"SatelliteDevnodeProbe\\Device") + L'\0';
    SwCreateInfo info{};
    info.cbSize = sizeof(info);
    info.pszInstanceId = instance.c_str();
    info.pszzHardwareIds = hardwareIds.c_str();
    info.CapabilityFlags = SW_CAPABILITIES_REMOVABLE | SW_CAPABILITIES_SILENT_INSTALL |
                           SW_CAPABILITIES_NO_DISPLAY_IN_UI;
    info.pszDeviceDescription = L"Satellite devnode probe";
    {
        std::lock_guard<std::mutex> lk(device.mtx);
        device.finished = false;
        device.result = E_PENDING;
    }
    const HRESULT started = api.create(ENUMERATOR, parent.c_str(), &info, 0, nullptr, onCreated,
                                       &device, &device.handle);
    if (FAILED(started)) {
        device.result = started;
        return false;
    }
    std::unique_lock<std::mutex> lk(device.mtx);
    const bool finished = device.done.wait_for(lk, STATE_BUDGET, [&] { return device.finished; });
    return finished && SUCCEEDED(device.result);
}

void closeDevice(const Api& api, SoftDevice& device) {
    if (device.handle != nullptr) api.close(device.handle);
    device.handle = nullptr;
}

// What CM_Locate_DevNode (normal) and CM_Get_DevNode_Status say of an id.
struct NodeState {
    CONFIGRET located = CR_FAILURE;
    CONFIGRET statused = CR_FAILURE;
    ULONG status = 0;
    ULONG problem = 0;

    bool present() const { return located == CR_SUCCESS; }
    bool started() const {
        return present() && statused == CR_SUCCESS && (status & DN_STARTED) != 0 &&
               (status & DN_HAS_PROBLEM) == 0;
    }
    bool hasProblem(ULONG code) const {
        return present() && statused == CR_SUCCESS && (status & DN_HAS_PROBLEM) != 0 &&
               problem == code;
    }
};

NodeState readNode(const std::wstring& instanceId) {
    NodeState state;
    DEVINST node = 0;
    std::wstring id = instanceId;
    state.located = CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL);
    if (state.located != CR_SUCCESS) return state;
    state.statused = CM_Get_DevNode_Status(&state.status, &state.problem, node, 0);
    return state;
}

std::string describe(const NodeState& state) {
    if (!state.present()) return "not located (CONFIGRET " + std::to_string(state.located) + ")";
    return "located, status 0x" + std::to_string(state.status) + " (started " +
           ((state.status & DN_STARTED) != 0 ? "yes" : "no") + "), problem " +
           std::to_string(state.problem);
}

template <typename Predicate>
NodeState waitForNode(const std::wstring& instanceId, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + STATE_BUDGET;
    NodeState state = readNode(instanceId);
    while (!predicate(state) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(POLL);
        state = readNode(instanceId);
    }
    return state;
}

// Device Manager's own way: DIF_PROPERTYCHANGE with a global scope, which
// keeps the device disabled until it is enabled the same way.
bool changeState(const std::wstring& instanceId, DWORD stateChange) {
    const HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    SP_PROPCHANGE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = stateChange;
    params.Scope = DICS_FLAG_GLOBAL;
    const bool opened = SetupDiOpenDeviceInfoW(set, instanceId.c_str(), nullptr, 0, &data) != 0;
    const bool staged = opened && SetupDiSetClassInstallParamsW(
                                      set, &data, &params.ClassInstallHeader, sizeof(params)) != 0;
    const bool called = staged && SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &data) != 0;
    const DWORD error = GetLastError();
    SetupDiDestroyDeviceInfoList(set);
    if (!called) std::cout << "probe: DIF_PROPERTYCHANGE failed, error " << error << "\n";
    return called;
}

std::vector<std::string> actionsFor(const std::vector<Event>& events, int registration,
                                    const std::wstring& instanceId) {
    std::vector<std::string> actions;
    for (const Event& e : events) {
        const bool matches = e.registration == registration && sameId(e.instanceId, instanceId);
        if (matches) actions.push_back(actionName(e.action));
    }
    return actions;
}

bool includes(const std::vector<std::string>& actions, const std::string& action) {
    return std::find(actions.begin(), actions.end(), action) != actions.end();
}

// The first child of a devnode by CM_Get_Child, named by CM_Get_Device_ID,
// the walk the helper takes from a pad's parent to its HID child.
struct ChildWalk {
    CONFIGRET found = CR_FAILURE;
    std::wstring instanceId;
};

ChildWalk firstChildOf(const std::wstring& parentId) {
    ChildWalk walk;
    DEVINST parent = 0;
    std::wstring id = parentId;
    walk.found = CM_Locate_DevNodeW(&parent, id.data(), CM_LOCATE_DEVNODE_NORMAL);
    if (walk.found != CR_SUCCESS) return walk;
    DEVINST child = 0;
    walk.found = CM_Get_Child(&child, parent, 0);
    if (walk.found != CR_SUCCESS) return walk;
    wchar_t name[MAX_DEVICE_ID_LEN] = {};
    walk.found = CM_Get_Device_IDW(child, name, MAX_DEVICE_ID_LEN, 0);
    walk.instanceId = name;
    return walk;
}

std::string joined(const std::vector<std::string>& actions) {
    if (actions.empty()) return "nothing";
    std::string out;
    for (const std::string& a : actions) out += (out.empty() ? "" : ", ") + a;
    return out;
}

enum RegistrationId { ALL_DEVICES = 0, PARENT = 1, CHILD = 2, CHILD_AGAIN = 3 };

struct Probe {
    Api api;
    Recorder recorder;
    SoftDevice parent;
    SoftDevice child;
    Registration all{ALL_DEVICES, &recorder};
    Registration onParent{PARENT, &recorder};
    Registration onChild{CHILD, &recorder};
    Registration onChildAgain{CHILD_AGAIN, &recorder};
    std::wstring childInstance;
};

void reportEvents(const char* step, const Probe& p, const std::vector<Event>& events) {
    std::cout << "probe: " << step << ": all-devices registration saw "
              << joined(actionsFor(events, ALL_DEVICES, p.parent.instanceId))
              << " for the parent and "
              << joined(actionsFor(events, ALL_DEVICES, p.child.instanceId))
              << " for the child; the "
              << "parent's own registration saw "
              << joined(actionsFor(events, PARENT, p.parent.instanceId)) << "; the child's saw "
              << joined(actionsFor(events, CHILD, p.child.instanceId)) << "\n";
}

void reportThreads(const Probe& p) {
    const std::vector<Event> events = p.recorder.since(0);
    std::set<DWORD> threads;
    for (const Event& e : events) threads.insert(e.thread);
    const bool onMain = threads.count(GetCurrentThreadId()) != 0;
    std::cout << "probe: " << events.size() << " callbacks ran on " << threads.size()
              << " distinct thread(s); the registering thread ran " << (onMain ? "some" : "none")
              << "\n";
    TEST("probe: a notification callback never runs on the thread that registered it");
    EXPECT(!onMain);
    EXPECT(!events.empty());
}

void probeDisable(Probe& p) {
    const size_t mark = p.recorder.mark();
    const bool viaDeviceManager = changeState(p.parent.instanceId, DICS_DISABLE);
    std::cout << "probe: disabled the parent through DIF_PROPERTYCHANGE: "
              << (viaDeviceManager ? "yes" : "no") << "\n";
    const NodeState parent = waitForNode(
        p.parent.instanceId, [](const NodeState& s) { return s.hasProblem(CM_PROB_DISABLED); });
    std::this_thread::sleep_for(SETTLE);
    const NodeState child = readNode(p.child.instanceId);
    const ChildWalk walk = firstChildOf(p.parent.instanceId);
    std::cout << "probe: disabled parent: " << describe(parent)
              << "; its child: " << describe(child) << "; CM_Get_Child returned " << walk.found
              << "\n";
    const std::vector<Event> events = p.recorder.since(mark);
    reportEvents("disable", p, events);

    TEST("probe: Device Manager's disable leaves the parent in the tree, found by a normal "
         "CM_Locate_DevNode, flagged DN_HAS_PROBLEM with CM_PROB_DISABLED (22)");
    EXPECT(viaDeviceManager);
    EXPECT(parent.hasProblem(CM_PROB_DISABLED));
    TEST("probe: a disabled parent's child leaves the tree with it, and the parent has no child "
         "to walk to");
    EXPECT(!child.present());
    EXPECT_EQ(walk.found, static_cast<CONFIGRET>(CR_NO_SUCH_DEVNODE));
    TEST("probe: SPEC 6.1: a disable sends no DEVICEINSTANCEREMOVED for the disabled devnode, "
         "which stays present, on its own registration or an all-devices one");
    EXPECT(!includes(actionsFor(events, PARENT, p.parent.instanceId), "REMOVED"));
    EXPECT(!includes(actionsFor(events, ALL_DEVICES, p.parent.instanceId), "REMOVED"));
    TEST("probe: SPEC 6.1: the child, which does leave the tree, gets DEVICEINSTANCEREMOVED on "
         "its own registration");
    EXPECT(includes(actionsFor(events, CHILD, p.child.instanceId), "REMOVED"));
}

void probeEnable(Probe& p) {
    const size_t mark = p.recorder.mark();
    const bool viaDeviceManager = changeState(p.parent.instanceId, DICS_ENABLE);
    const NodeState parent =
        waitForNode(p.parent.instanceId, [](const NodeState& s) { return s.started(); });
    const NodeState child =
        waitForNode(p.child.instanceId, [](const NodeState& s) { return s.started(); });
    std::this_thread::sleep_for(SETTLE);
    std::cout << "probe: enabled parent: " << describe(parent) << "; its child: " << describe(child)
              << "\n";
    const std::vector<Event> events = p.recorder.since(mark);
    reportEvents("enable", p, events);
    TEST("probe: enabling the parent sends DEVICEINSTANCESTARTED on its own registration");
    EXPECT(includes(actionsFor(events, PARENT, p.parent.instanceId), "STARTED"));
    TEST("probe: SPEC 6.2: the child's registration, made before it left with its parent, sees "
         "it start again");
    EXPECT(includes(actionsFor(events, CHILD, p.child.instanceId), "STARTED"));

    TEST("probe: enabling the parent the same way starts it again, and its child comes back "
         "started under the same instance id");
    EXPECT(viaDeviceManager);
    EXPECT(parent.started());
    EXPECT(child.started());
}

void probeLeaveAndReturn(Probe& p) {
    const size_t mark = p.recorder.mark();
    closeDevice(p.api, p.child);
    const NodeState gone =
        waitForNode(p.child.instanceId, [](const NodeState& s) { return !s.present(); });
    std::this_thread::sleep_for(SETTLE);
    std::cout << "probe: closed child: " << describe(gone) << "\n";
    const std::vector<Event> removal = p.recorder.since(mark);
    reportEvents("child removed", p, removal);

    registerFor(p.api, p.onChildAgain, instanceFilter(p.child.instanceId));
    std::cout << "probe: a new registration for the absent child's id returned CONFIGRET "
              << p.onChildAgain.result << "\n";

    const size_t back = p.recorder.mark();
    const bool recreated = createDevice(p.api, p.child, p.parent.instanceId, p.childInstance);
    const NodeState returned =
        waitForNode(p.child.instanceId, [](const NodeState& s) { return s.started(); });
    std::this_thread::sleep_for(SETTLE);
    std::cout << "probe: re-created child (" << (recreated ? "ok" : "failed")
              << "): " << describe(returned) << "\n";
    const std::vector<Event> events = p.recorder.since(back);
    reportEvents("child re-created", p, events);
    std::cout << "probe: the registration made while it was absent saw "
              << joined(actionsFor(events, CHILD_AGAIN, p.child.instanceId)) << "\n";

    TEST("probe: a removed device is no longer found by a normal CM_Locate_DevNode");
    EXPECT_EQ(gone.located, static_cast<CONFIGRET>(CR_NO_SUCH_DEVNODE));
    TEST("probe: the same instance id can be created again and starts");
    EXPECT(recreated);
    EXPECT(returned.started());
    TEST("probe: SPEC 6.2: a registration for an id that has no devnode is refused");
    EXPECT(p.onChildAgain.result != CR_SUCCESS);
    TEST("probe: SPEC 6.2: the child's first registration sees its removal and then the device "
         "made again under the same id");
    EXPECT(includes(actionsFor(removal, CHILD, p.child.instanceId), "REMOVED"));
    EXPECT(includes(actionsFor(events, CHILD, p.child.instanceId), "STARTED"));
}

// The CM call the helper's selftest disables a pad with, beside Device
// Manager's DIF_PROPERTYCHANGE.
void probeDisableThroughCm(Probe& p) {
    DEVINST node = 0;
    std::wstring id = p.parent.instanceId;
    const CONFIGRET located = CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL);
    const CONFIGRET disabled = CM_Disable_DevNode(node, CM_DISABLE_UI_NOT_OK);
    const NodeState off = waitForNode(
        p.parent.instanceId, [](const NodeState& s) { return s.hasProblem(CM_PROB_DISABLED); });
    const CONFIGRET enabled = CM_Enable_DevNode(node, 0);
    const NodeState parent =
        waitForNode(p.parent.instanceId, [](const NodeState& s) { return s.started(); });
    const NodeState child =
        waitForNode(p.child.instanceId, [](const NodeState& s) { return s.started(); });
    std::cout << "probe: CM_Disable_DevNode " << disabled << ": " << describe(off)
              << "; CM_Enable_DevNode " << enabled << ": " << describe(parent) << "; child "
              << describe(child) << "\n";

    TEST("probe: CM_Disable_DevNode leaves the CM_PROB_DISABLED (22) Device Manager's disable "
         "leaves, and CM_Enable_DevNode starts the parent and its child again");
    EXPECT_EQ(located, static_cast<CONFIGRET>(CR_SUCCESS));
    EXPECT_EQ(disabled, static_cast<CONFIGRET>(CR_SUCCESS));
    EXPECT(off.hasProblem(CM_PROB_DISABLED));
    EXPECT_EQ(enabled, static_cast<CONFIGRET>(CR_SUCCESS));
    EXPECT(parent.started());
    EXPECT(child.started());
}

void probeChildWalk(const Probe& p) {
    const ChildWalk walk = firstChildOf(p.parent.instanceId);
    std::cout << "probe: CM_Get_Child of the parent: " << walk.found << ", "
              << narrow(walk.instanceId) << "\n";
    TEST("probe: CM_Get_Child walks from the parent to its child, and CM_Get_Device_ID names it");
    EXPECT_EQ(walk.found, static_cast<CONFIGRET>(CR_SUCCESS));
    EXPECT(sameId(walk.instanceId, p.child.instanceId));
}

bool lowerToMediumIntegrity(HANDLE token) {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID medium = nullptr;
    if (!AllocateAndInitializeSid(&authority, 1, SECURITY_MANDATORY_MEDIUM_RID, 0, 0, 0, 0, 0, 0, 0,
                                  &medium)) {
        return false;
    }
    TOKEN_MANDATORY_LABEL label{};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = medium;
    const DWORD size = static_cast<DWORD>(sizeof(label)) + GetLengthSid(medium);
    const bool lowered = SetTokenInformation(token, TokenIntegrityLevel, &label, size) != 0;
    FreeSid(medium);
    return lowered;
}

// A filtered, medium-integrity copy of this process's token, the token a
// non-elevated satellite.exe runs with, to impersonate on a thread of its own.
HANDLE unelevatedToken() {
    HANDLE process = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
                              TOKEN_ASSIGN_PRIMARY,
                          &process)) {
        return nullptr;
    }
    HANDLE restricted = nullptr;
    const BOOL made = CreateRestrictedToken(process, LUA_TOKEN_FLAG, 0, nullptr, 0, nullptr, 0,
                                            nullptr, &restricted);
    CloseHandle(process);
    if (!made) return nullptr;
    HANDLE impersonation = nullptr;
    const bool usable =
        lowerToMediumIntegrity(restricted) &&
        DuplicateTokenEx(restricted, TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr,
                         SecurityImpersonation, TokenImpersonation, &impersonation) != 0;
    CloseHandle(restricted);
    return usable ? impersonation : nullptr;
}

struct UnelevatedFindings {
    bool impersonated = false;
    bool elevated = true;
    CONFIGRET located = CR_FAILURE;
    CONFIGRET statused = CR_FAILURE;
    CONFIGRET registered = CR_FAILURE;
    CONFIGRET disabled = CR_FAILURE;
};

void runUnelevated(const Api& api, Recorder& recorder, const std::wstring& instanceId,
                   UnelevatedFindings& out) {
    HANDLE token = unelevatedToken();
    if (token == nullptr) return;
    out.impersonated = SetThreadToken(nullptr, token) != 0;
    out.elevated = tokenElevated(token);
    DEVINST node = 0;
    std::wstring id = instanceId;
    out.located = CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL);
    ULONG status = 0;
    ULONG problem = 0;
    out.statused = CM_Get_DevNode_Status(&status, &problem, node, 0);
    Registration registration{PARENT, &recorder};
    registerFor(api, registration, instanceFilter(instanceId));
    out.registered = registration.result;
    unregister(api, registration);
    out.disabled = CM_Disable_DevNode(node, CM_DISABLE_UI_NOT_OK);
    RevertToSelf();
    CloseHandle(token);
    if (out.disabled == CR_SUCCESS) CM_Enable_DevNode(node, 0);
}

void probeUnelevated(Probe& p) {
    UnelevatedFindings found;
    std::thread worker([&] { runUnelevated(p.api, p.recorder, p.parent.instanceId, found); });
    worker.join();
    std::cout << "probe: unelevated thread: impersonated " << found.impersonated << ", elevated "
              << found.elevated << ", locate " << found.located << ", status " << found.statused
              << ", register " << found.registered << ", disable " << found.disabled << "\n";

    TEST("probe: a filtered, medium-integrity token locates a devnode, reads its status and "
         "registers for its notifications, and is refused a disable");
    EXPECT(found.impersonated);
    EXPECT(!found.elevated);
    EXPECT_EQ(found.located, static_cast<CONFIGRET>(CR_SUCCESS));
    EXPECT_EQ(found.statused, static_cast<CONFIGRET>(CR_SUCCESS));
    EXPECT_EQ(found.registered, static_cast<CONFIGRET>(CR_SUCCESS));
    EXPECT(found.disabled != CR_SUCCESS);
}

void uninstallPhantom(const std::wstring& instanceId) {
    DEVINST node = 0;
    std::wstring id = instanceId;
    if (CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS) {
        CM_Uninstall_DevNode(node, 0);
    }
}

void cleanUp(Probe& p) {
    for (Registration* r : {&p.all, &p.onParent, &p.onChild, &p.onChildAgain})
        unregister(p.api, *r);
    closeDevice(p.api, p.child);
    closeDevice(p.api, p.parent);
    for (const std::wstring& id : {p.child.instanceId, p.parent.instanceId}) {
        if (!id.empty()) uninstallPhantom(id);
    }
}

int skipOrFail(const char* why) {
    std::cout << "windows_devnode_probe: cannot run: " << why << "\n";
    if (!onGitHubActions()) return SKIPPED;
    std::cout << "windows_devnode_probe: GitHub Actions runs this probe, so a skip there fails\n";
    return 1;
}

int runProbe(Probe& p) {
    const std::wstring stamp =
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    p.childInstance = L"child-" + stamp;
    TEST("probe: SwDeviceCreate makes a driverless software device that starts raw, and a "
         "child below it");
    const bool parentMade = createDevice(p.api, p.parent, ROOT_PARENT, L"parent-" + stamp);
    std::cout << "probe: parent SwDeviceCreate result 0x" << std::hex << p.parent.result << std::dec
              << ", instance " << narrow(p.parent.instanceId) << "\n";
    EXPECT(parentMade);
    if (!parentMade) return 1;
    const NodeState parent =
        waitForNode(p.parent.instanceId, [](const NodeState& s) { return s.started(); });
    const bool childMade = createDevice(p.api, p.child, p.parent.instanceId, p.childInstance);
    std::cout << "probe: child SwDeviceCreate result 0x" << std::hex << p.child.result << std::dec
              << ", instance " << narrow(p.child.instanceId) << "\n";
    EXPECT(childMade);
    if (!childMade) return 1;
    const NodeState child =
        waitForNode(p.child.instanceId, [](const NodeState& s) { return s.started(); });
    std::cout << "probe: parent " << describe(parent) << "; child " << describe(child) << "\n";
    EXPECT(parent.started());
    EXPECT(child.started());

    registerFor(p.api, p.all, allInstancesFilter());
    registerFor(p.api, p.onParent, instanceFilter(p.parent.instanceId));
    registerFor(p.api, p.onChild, instanceFilter(p.child.instanceId));
    TEST("probe: an all-devices and a per-id registration both succeed");
    EXPECT_EQ(p.all.result, static_cast<CONFIGRET>(CR_SUCCESS));
    EXPECT_EQ(p.onParent.result, static_cast<CONFIGRET>(CR_SUCCESS));
    EXPECT_EQ(p.onChild.result, static_cast<CONFIGRET>(CR_SUCCESS));

    probeChildWalk(p);
    probeDisable(p);
    probeEnable(p);
    probeDisableThroughCm(p);
    probeLeaveAndReturn(p);
    probeUnelevated(p);
    reportThreads(p);
    return 0;
}

} // namespace

int main() {
    const Api api = resolveApi();
    if (underWine()) return skipOrFail("Wine has no Plug and Play manager to probe");
    if (!api.complete())
        return skipOrFail("cfgmgr32 lacks SwDeviceCreate or CM_Register_Notification");
    if (!processElevated()) return skipOrFail("software devices need an elevated process");

    Probe probe;
    probe.api = api;
    const int ran = runProbe(probe);
    cleanUp(probe);
    std::cout << "windows_devnode_probe: " << g_pass << " passed, " << g_fail << " failed\n";
    return ran == 0 && g_fail == 0 ? 0 : 1;
}
