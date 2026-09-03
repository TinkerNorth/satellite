// SPDX-License-Identifier: LGPL-3.0-or-later
#include "hidmaestro_helper_client.h"

#include "core/driver_inf.h"
#include "core/json.h"
#include "core/semver.h"
#include "hidmaestro_report.h"

#include <shellapi.h>

#include <fstream>
#include <random>

namespace satellite {
namespace hidmaestro {

namespace {

// Cold driver deploys inside a plug can run tens of seconds on slow machines;
// the connect budget additionally covers a user hesitating at the UAC prompt.
constexpr DWORD kConnectTimeoutMs = 120000;
constexpr DWORD kRequestTimeoutMs = 120000;

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

    // Unguessable per-session pipe name; the connecting client's PID is
    // verified against the process we spawned before any request is sent.
    std::random_device rd;
    wchar_t token[33];
    for (int i = 0; i < 32; ++i) token[i] = L"0123456789abcdef"[rd() & 0xF];
    token[32] = L'\0';
    std::wstring pipeName =
        L"\\\\.\\pipe\\satellite-hm-" + std::to_wstring(GetCurrentProcessId()) + L"-" + token;

    HANDLE pipe = CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;

    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ioEvent == nullptr) {
        CloseHandle(pipe);
        return false;
    }

    std::wstring params = L"serve --pipe \"" + pipeName + L"\" --parent-pid " +
                          std::to_wstring(GetCurrentProcessId());

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas"; // no-op prompt-free when satellite itself is elevated
    sei.lpFile = helper.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        CloseHandle(ioEvent);
        CloseHandle(pipe);
        return false;
    }

    OVERLAPPED ov{};
    ov.hEvent = ioEvent;
    bool connected = ConnectNamedPipe(pipe, &ov) != 0;
    if (!connected) {
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (err == ERROR_IO_PENDING) {
            HANDLE waits[2] = {ioEvent, sei.hProcess};
            const DWORD rc = WaitForMultipleObjects(2, waits, FALSE, kConnectTimeoutMs);
            if (rc == WAIT_OBJECT_0) {
                DWORD ignored = 0;
                connected = GetOverlappedResult(pipe, &ov, &ignored, FALSE) != 0;
            } else {
                // Helper exited (UAC declined, crash) or timed out.
                CancelIoEx(pipe, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &ov, &ignored, TRUE);
            }
        }
    }

    ULONG clientPid = 0;
    if (connected && (!GetNamedPipeClientProcessId(pipe, &clientPid) ||
                      clientPid != GetProcessId(sei.hProcess))) {
        connected = false; // someone else raced onto our pipe name: refuse it
    }

    if (!connected) {
        TerminateProcess(sei.hProcess, 1);
        CloseHandle(sei.hProcess);
        CloseHandle(ioEvent);
        CloseHandle(pipe);
        return false;
    }

    pipe_ = pipe;
    ioEvent_ = ioEvent;
    helperProcess_ = sei.hProcess;

    std::string response;
    if (!requestLocked("{\"op\":\"hello\",\"protocol\":1}", response)) {
        stopLocked(false);
        return false;
    }
    Json j;
    if (!jsonParse(response, j) || !jsonBool(j, "ok")) {
        stopLocked(false);
        return false;
    }
    return true;
}

void HelperClient::stopLocked(bool sendShutdown) {
    if (pipe_ != INVALID_HANDLE_VALUE && sendShutdown) {
        std::string response;
        requestLocked("{\"op\":\"shutdown\"}", response);
        WaitForSingleObject(helperProcess_, 5000);
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

// Caller holds mtx_. One newline-terminated JSON request, one newline-
// terminated JSON response; any transport failure tears the channel down so
// the next ensureReady() starts a fresh helper.
bool HelperClient::requestLocked(const std::string& line, std::string& response) {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    auto fail = [this]() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        if (ioEvent_) CloseHandle(ioEvent_);
        ioEvent_ = nullptr;
        if (helperProcess_) {
            TerminateProcess(helperProcess_, 1);
            CloseHandle(helperProcess_);
            helperProcess_ = nullptr;
        }
        return false;
    };

    auto overlappedIo = [this](bool write, void* buf, DWORD len, DWORD& moved) {
        OVERLAPPED ov{};
        ov.hEvent = ioEvent_;
        ResetEvent(ioEvent_);
        const BOOL started = write ? WriteFile(pipe_, buf, len, nullptr, &ov)
                                   : ReadFile(pipe_, buf, len, nullptr, &ov);
        if (!started && GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(ioEvent_, kRequestTimeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(pipe_, &ov);
            GetOverlappedResult(pipe_, &ov, &moved, TRUE);
            return false;
        }
        return GetOverlappedResult(pipe_, &ov, &moved, FALSE) != 0;
    };

    std::string out = line;
    out.push_back('\n');
    DWORD moved = 0;
    if (!overlappedIo(true, out.data(), static_cast<DWORD>(out.size()), moved) ||
        moved != out.size()) {
        return fail();
    }

    response.clear();
    char ch = 0;
    while (true) {
        if (!overlappedIo(false, &ch, 1, moved) || moved != 1) return fail();
        if (ch == '\n') break;
        if (ch != '\r') response.push_back(ch);
        if (response.size() > 64 * 1024) return fail();
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
