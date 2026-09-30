// SPDX-License-Identifier: LGPL-3.0-or-later
#include "hidmaestro_helper_client.h"

#include "app/app_state.h"
#include "core/driver_inf.h"
#include "core/hex.h"
#include "core/json.h"
#include "core/semver.h"
#include "hidmaestro_report.h"

#include <shellapi.h>
#include <winsvc.h>

#include <fstream>
#include <random>

namespace satellite {
namespace hidmaestro {

namespace {

// Cold driver deploys inside a plug can run tens of seconds on slow machines;
// the connect budget additionally covers a user hesitating at the UAC prompt.
constexpr DWORD kConnectTimeoutMs = 120000;
constexpr DWORD kRequestTimeoutMs = 120000;
constexpr DWORD kBrokerConnectTimeoutMs = 10000;
constexpr const wchar_t* kBrokerServiceName = L"SatelliteHmBroker";
constexpr const wchar_t* kBrokerPipe = L"\\\\.\\pipe\\satellite-hm-broker";

std::wstring modulePathDirFile(const wchar_t* filename) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring path(buf, n);
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L"";
    return path.substr(0, slash + 1) + filename;
}

std::wstring driverStoreRepository() {
    wchar_t sysRoot[MAX_PATH];
    const UINT n = GetSystemWindowsDirectoryW(sysRoot, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(sysRoot, n) + L"\\System32\\DriverStore\\FileRepository\\";
}

bool driverStorePresent() {
    const std::wstring repo = driverStoreRepository();
    if (repo.empty()) return false;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((repo + L"hidmaestro.inf_*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find);
    return true;
}

DWORD brokerServicePid(bool tryStart) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) return 0;
    DWORD access = SERVICE_QUERY_STATUS | (tryStart ? SERVICE_START : 0);
    SC_HANDLE svc = OpenServiceW(scm, kBrokerServiceName, access);
    if (svc == nullptr && tryStart)
        svc = OpenServiceW(scm, kBrokerServiceName, SERVICE_QUERY_STATUS);
    if (svc == nullptr) {
        CloseServiceHandle(scm);
        return 0;
    }
    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    DWORD pid = 0;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp),
                             sizeof(ssp), &needed)) {
        if (ssp.dwCurrentState == SERVICE_STOPPED && tryStart) StartServiceW(svc, 0, nullptr);
        if (ssp.dwCurrentState == SERVICE_RUNNING) pid = ssp.dwProcessId;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return pid;
}

std::string readFileBytes(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

std::wstring helperBinaryPath() { return modulePathDirFile(L"satellite-hm-helper.exe"); }

bool helperBinaryPresent() {
    const std::wstring path = helperBinaryPath();
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool brokerServiceRegistered() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) return false;
    SC_HANDLE svc = OpenServiceW(scm, kBrokerServiceName, SERVICE_QUERY_STATUS);
    const bool exists = svc != nullptr;
    if (svc != nullptr) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return exists;
}

bool driverInstalled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\HIDMaestro", 0, KEY_READ | KEY_WOW64_64KEY,
                      &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return driverStorePresent();
}

std::string installedDriverVersion() {
    const std::wstring repo = driverStoreRepository();
    if (repo.empty()) return "";
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((repo + L"hidmaestro.inf_*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return "";
    std::string best;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        const std::string v =
            parseInfDriverVersion(readFileBytes(repo + fd.cFileName + L"\\hidmaestro.inf"));
        if (v.empty()) continue;
        if (best.empty() || compareDottedVersion(v, best) > 0) best = v;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return best;
}

bool installDriver(std::string& outError) {
    const std::wstring helper = helperBinaryPath();
    if (helper.empty() || GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) {
        outError = "satellite-hm-helper.exe is not next to satellite.exe";
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = helper.c_str();
    sei.lpParameters = L"install-driver";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        const DWORD err = GetLastError();
        outError = err == ERROR_CANCELLED
                       ? "Elevation was declined"
                       : "Could not start the helper (error " + std::to_string(err) + ")";
        return false;
    }

    const DWORD waited = WaitForSingleObject(sei.hProcess, kRequestTimeoutMs);
    DWORD exitCode = 0;
    if (waited != WAIT_OBJECT_0) {
        CloseHandle(sei.hProcess);
        outError = "The driver install did not finish in time";
        return false;
    }
    const bool gotExit = GetExitCodeProcess(sei.hProcess, &exitCode) != 0;
    CloseHandle(sei.hProcess);
    if (!gotExit || exitCode != 0) {
        outError = "The helper reported failure (exit " + std::to_string(exitCode) + ")";
        return false;
    }
    if (!driverInstalled()) {
        outError = "The helper finished but the driver is still not installed";
        return false;
    }
    return true;
}

HelperClient::~HelperClient() { shutdown(); }

bool HelperClient::installed() const { return helperBinaryPresent() && driverInstalled(); }

bool HelperClient::isReady() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pipe_ != INVALID_HANDLE_VALUE;
}

bool HelperClient::ensureReady() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    return startLocked();
}

void HelperClient::shutdown() {
    std::lock_guard<std::mutex> lk(mtx_);
    stopLocked(/*sendShutdown=*/true);
}

bool HelperClient::startLocked() {
    const std::wstring helper = helperBinaryPath();
    if (helper.empty() || GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    if (connectBrokerLocked()) {
        logMsg(LogLevel::INFO, "hidmaestro", "Connected to the SatelliteHmBroker service");
        return true;
    }
    if (!spawnHelperLocked()) return false;
    logMsg(LogLevel::INFO, "hidmaestro", "Spawned an elevated helper for this session");
    return true;
}

bool HelperClient::connectBrokerLocked() {
    if (!brokerServiceRegistered()) return false;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    const ULONGLONG deadline = GetTickCount64() + kBrokerConnectTimeoutMs;
    bool startAttempted = false;
    while (true) {
        pipe = CreateFileW(kBrokerPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(kBrokerPipe, 1000);
        } else if (err == ERROR_FILE_NOT_FOUND) {
            if (!startAttempted) {
                brokerServicePid(/*tryStart=*/true);
                startAttempted = true;
            }
            Sleep(250);
        } else {
            return false;
        }
        if (GetTickCount64() >= deadline) return false;
    }

    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid == 0 ||
        serverPid != brokerServicePid(/*tryStart=*/false)) {
        CloseHandle(pipe);
        return false;
    }

    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ioEvent == nullptr) {
        CloseHandle(pipe);
        return false;
    }
    pipe_ = pipe;
    ioEvent_ = ioEvent;
    helperProcess_ = nullptr;
    if (!helloLocked()) {
        stopLocked(false);
        return false;
    }
    return true;
}

bool HelperClient::helloLocked() {
    std::string response;
    if (!requestLocked("{\"op\":\"hello\",\"protocol\":1}", response)) return false;
    Json j;
    return jsonParse(response, j) && jsonBool(j, "ok");
}

namespace {

constexpr size_t kPipeTokenBytes = 16;

// Unguessable per-session pipe name; the connecting client's PID is verified
// against the process we spawned before any request is sent.
std::wstring freshPipeName() {
    std::random_device rd;
    uint8_t tokenBytes[kPipeTokenBytes];
    for (uint8_t& byte : tokenBytes) byte = static_cast<uint8_t>(rd());
    const std::string token = hexEncode(tokenBytes, sizeof(tokenBytes));
    const std::wstring wideToken(token.begin(), token.end());
    return L"\\\\.\\pipe\\satellite-hm-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           wideToken;
}

// The helper process serving `pipeName`, or nullptr when it would not start.
// "runas" is a prompt-free no-op when satellite itself is elevated.
HANDLE launchHelper(const std::wstring& helper, const std::wstring& pipeName) {
    const std::wstring params = L"serve --pipe \"" + pipeName + L"\" --parent-pid " +
                                std::to_wstring(GetCurrentProcessId());
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = helper.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return nullptr;
    return sei.hProcess;
}

// Waits for a client to connect to `pipe`, giving up when the helper exits
// (UAC declined, crash) or the connect budget runs out.
bool awaitPipeClient(HANDLE pipe, HANDLE ioEvent, HANDLE process) {
    OVERLAPPED ov{};
    ov.hEvent = ioEvent;
    if (ConnectNamedPipe(pipe, &ov) != 0) return true;
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) return true;
    if (err != ERROR_IO_PENDING) return false;
    HANDLE waits[2] = {ioEvent, process};
    const DWORD rc = WaitForMultipleObjects(2, waits, FALSE, kConnectTimeoutMs);
    DWORD ignored = 0;
    if (rc == WAIT_OBJECT_0) return GetOverlappedResult(pipe, &ov, &ignored, FALSE) != 0;
    CancelIoEx(pipe, &ov);
    GetOverlappedResult(pipe, &ov, &ignored, TRUE);
    return false;
}

// Someone else racing onto our pipe name is refused.
bool pipeClientIs(HANDLE pipe, HANDLE process) {
    ULONG clientPid = 0;
    return GetNamedPipeClientProcessId(pipe, &clientPid) && clientPid == GetProcessId(process);
}

} // namespace

bool HelperClient::spawnHelperLocked() {
    const std::wstring pipeName = freshPipeName();
    HANDLE pipe = CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;

    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ioEvent == nullptr) {
        CloseHandle(pipe);
        return false;
    }

    HANDLE process = launchHelper(helperBinaryPath(), pipeName);
    if (process == nullptr) {
        CloseHandle(ioEvent);
        CloseHandle(pipe);
        return false;
    }

    const bool connected = awaitPipeClient(pipe, ioEvent, process) && pipeClientIs(pipe, process);
    if (!connected) {
        TerminateProcess(process, 1);
        CloseHandle(process);
        CloseHandle(ioEvent);
        CloseHandle(pipe);
        return false;
    }

    pipe_ = pipe;
    ioEvent_ = ioEvent;
    helperProcess_ = process;

    if (!helloLocked()) {
        stopLocked(false);
        return false;
    }
    return true;
}

void HelperClient::stopLocked(bool sendShutdown) {
    if (pipe_ != INVALID_HANDLE_VALUE && sendShutdown) {
        std::string response;
        requestLocked("{\"op\":\"shutdown\"}", response);
        if (helperProcess_) WaitForSingleObject(helperProcess_, 5000);
    }
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    if (ioEvent_) CloseHandle(ioEvent_);
    ioEvent_ = nullptr;
    if (helperProcess_) {
        // The helper self-heals orphans on its next launch, so a wedged one is
        // safe to kill rather than wait on.
        if (WaitForSingleObject(helperProcess_, 0) == WAIT_TIMEOUT)
            TerminateProcess(helperProcess_, 1);
        CloseHandle(helperProcess_);
        helperProcess_ = nullptr;
    }
}

// Caller holds mtx_. One overlapped read or write on the pipe, bounded by the
// request budget; `moved` is what completed.
bool HelperClient::overlappedIoLocked(bool write, void* buf, DWORD len, DWORD& moved) {
    OVERLAPPED ov{};
    ov.hEvent = ioEvent_;
    ResetEvent(ioEvent_);
    const BOOL started =
        write ? WriteFile(pipe_, buf, len, nullptr, &ov) : ReadFile(pipe_, buf, len, nullptr, &ov);
    if (!started && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(ioEvent_, kRequestTimeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(pipe_, &ov);
        GetOverlappedResult(pipe_, &ov, &moved, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe_, &ov, &moved, FALSE) != 0;
}

// Caller holds mtx_. Tears the channel down so the next ensureReady() starts a
// fresh helper; always false, so a failing request can return it.
bool HelperClient::dropChannelLocked() {
    stopLocked(false);
    return false;
}

// Caller holds mtx_. One newline-terminated JSON request, one newline-
// terminated JSON response; any transport failure drops the channel.
bool HelperClient::requestLocked(const std::string& line, std::string& response) {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    std::string out = line;
    out.push_back('\n');
    DWORD moved = 0;
    const bool sent = overlappedIoLocked(true, out.data(), static_cast<DWORD>(out.size()), moved) &&
                      moved == out.size();
    if (!sent) return dropChannelLocked();

    response.clear();
    char ch = 0;
    while (true) {
        if (!overlappedIoLocked(false, &ch, 1, moved) || moved != 1) return dropChannelLocked();
        if (ch == '\n') break;
        if (ch != '\r') response.push_back(ch);
        if (response.size() > 64 * 1024) return dropChannelLocked();
    }
    return true;
}

bool HelperClient::provision(uint32_t serial, GamepadIdentity identity, bool audio,
                             ProvisionResult& out) {
    const char* profile = profileForIdentity(identity, audio);
    if (profile == nullptr) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    JsonOut req;
    req["op"] = "plug";
    req["serial"] = serial;
    req["profile"] = profile;
    std::string response;
    if (!requestLocked(jsonDump(req), response)) return false;

    Json j;
    if (!jsonParse(response, j) || !jsonBool(j, "ok")) return false;
    auto handleField = [&j](const char* key) -> uint64_t {
        int64_t v = 0;
        return jsonTryI64(j, key, v) && v > 0 ? static_cast<uint64_t>(v) : 0;
    };
    out.controllerIndex = static_cast<uint32_t>(jsonInt(j, "index", 0));
    out.inputSection = handleField("input");
    out.inputEvent = handleField("inputEvent");
    out.companionEvent = handleField("companionEvent");
    out.outputSection = handleField("output");
    out.outputEvent = handleField("outputEvent");

    // Audio fields are absent on a plain profile and on a helper that predates
    // controller audio, so every one of them defaults to "no endpoint" rather
    // than to a guess about the persona's format.
    out.speakerSection = handleField("speakerAudio");
    out.speakerEvent = handleField("speakerAudioEvent");
    out.micSection = handleField("micAudio");
    out.micEvent = handleField("micAudioEvent");
    out.hapticSection = handleField("hapticAudio");
    out.hapticEvent = handleField("hapticAudioEvent");
    out.speakerChannels = static_cast<int>(jsonInt(j, "speakerChannels", 0));
    out.speakerRateHz = static_cast<int>(jsonInt(j, "speakerRateHz", 0));
    out.micChannels = static_cast<int>(jsonInt(j, "micChannels", 0));
    out.micRateHz = static_cast<int>(jsonInt(j, "micRateHz", 0));

    return out.inputSection != 0 && out.inputEvent != 0;
}

bool HelperClient::deprovision(uint32_t serial) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    JsonOut req;
    req["op"] = "unplug";
    req["serial"] = serial;
    std::string response;
    if (!requestLocked(jsonDump(req), response)) return false;
    Json j;
    return jsonParse(response, j) && jsonBool(j, "ok");
}

} // namespace hidmaestro
} // namespace satellite
