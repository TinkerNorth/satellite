// SPDX-License-Identifier: LGPL-3.0-or-later
#include "hidmaestro_helper_client.h"

#include "app/app_state.h"
#include "core/driver_inf.h"
#include "core/hex.h"
#include "core/hidmaestro_device_watch.h"
#include "core/json.h"
#include "core/semver.h"
#include "hidmaestro_report.h"

#include <shellapi.h>
#include <winsvc.h>

#include <algorithm>
#include <deque>
#include <utility>
#include <fstream>
#include <memory>
#include <random>
#include <vector>

namespace satellite {
namespace hidmaestro {

namespace {

std::atomic<HelperClient*> g_connectOnInstall{nullptr};

#ifdef SATELLITE_BUILD_TESTS
std::atomic<uint64_t> g_pipeReads{0};
void countPipeRead() { g_pipeReads.fetch_add(1, std::memory_order_relaxed); }
#else
void countPipeRead() {}
#endif

void notifyDriverInstalled() {
    if (HelperClient* client = g_connectOnInstall.load(std::memory_order_acquire)) {
        client->connectBrokerInBackground();
    }
}

constexpr DWORD kInstallTimeoutMs = 120000;
constexpr size_t kReplyLimitBytes = 64 * 1024;
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
    blockingRegion("helper.pipe");
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        const DWORD err = GetLastError();
        outError = err == ERROR_CANCELLED
                       ? "Elevation was declined"
                       : "Could not start the helper (error " + std::to_string(err) + ")";
        return false;
    }

    const DWORD waited = WaitForSingleObject(sei.hProcess, kInstallTimeoutMs);
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
    notifyDriverInstalled();
    return true;
}

struct HelperChannel {
    HelperChannel() = default;
    HelperChannel(const HelperChannel&) = delete;
    HelperChannel& operator=(const HelperChannel&) = delete;
    ~HelperChannel() { close(); }

    bool attach(HANDLE pipeHandle, HANDLE processHandle) {
        close();
        pipe = pipeHandle;
        process = processHandle;
        ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ioEvent == nullptr) {
            close();
            return false;
        }
        chunk.resize(kReplyLimitBytes);
        return true;
    }

    void close() {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
        if (ioEvent != nullptr) CloseHandle(ioEvent);
        ioEvent = nullptr;
        if (process != nullptr) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 1);
            CloseHandle(process);
            process = nullptr;
        }
        pending.clear();
    }

    bool alive() const {
        return pipe != INVALID_HANDLE_VALUE &&
               PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr) != 0;
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE ioEvent = nullptr;
    HANDLE process = nullptr;
    std::string pending;
    std::vector<char> chunk;
    // The serials whose lost-pad unplug went without its answer yet, in the
    // order sent: those answers come ahead of any later request's.
    struct Owed {
        uint32_t serial;
        // Past this, an answer still missing is a helper that no longer answers.
        ULONGLONG due;
    };
    std::deque<Owed> owed;
    // What the hello settled on; the base protocol hands over no watch blocks.
    int32_t protocol = HELPER_PROTOCOL_BASE;
};

namespace {

struct IoBudget {
    ULONGLONG deadline;
    HANDLE cancel;
};

IoBudget budgetFor(DWORD waitMs, HANDLE cancel) { return {GetTickCount64() + waitMs, cancel}; }

DWORD msLeft(const IoBudget& budget) {
    const ULONGLONG now = GetTickCount64();
    return budget.deadline > now ? static_cast<DWORD>(budget.deadline - now) : 0;
}

#ifdef SATELLITE_BUILD_TESTS
std::function<void()> g_beforeCancel;

void runBeforeCancelForTest() {
    std::function<void()> hook = std::exchange(g_beforeCancel, nullptr);
    if (hook) hook();
}
#endif

bool pipeIo(HelperChannel& ch, bool write, void* buf, DWORD len, DWORD& moved,
            const IoBudget& budget) {
    OVERLAPPED ov{};
    ov.hEvent = ch.ioEvent;
    ResetEvent(ch.ioEvent);
    const BOOL started = write ? WriteFile(ch.pipe, buf, len, nullptr, &ov)
                               : ReadFile(ch.pipe, buf, len, nullptr, &ov);
    if (!started && GetLastError() != ERROR_IO_PENDING) return false;
    blockingRegion("helper.pipe");
    if (!write) countPipeRead();
    HANDLE waits[2] = {ch.ioEvent, budget.cancel};
    const DWORD count = budget.cancel != nullptr ? 2 : 1;
    if (WaitForMultipleObjects(count, waits, FALSE, msLeft(budget)) != WAIT_OBJECT_0) {
#ifdef SATELLITE_BUILD_TESTS
        runBeforeCancelForTest();
#endif
        CancelIoEx(ch.pipe, &ov);
        // The I/O can finish between the wait running out and the cancel:
        // what it moved is moved, and an answer's bytes are kept.
        const BOOL finished = GetOverlappedResult(ch.pipe, &ov, &moved, TRUE);
        return finished != 0 && moved > 0;
    }
    return GetOverlappedResult(ch.pipe, &ov, &moved, FALSE) != 0;
}

bool readLine(HelperChannel& ch, std::string& line, const IoBudget& budget) {
    size_t scanned = 0;
    for (;;) {
        const size_t end = ch.pending.find('\n', scanned);
        if (end != std::string::npos) {
            line.assign(ch.pending, 0, end);
            ch.pending.erase(0, end + 1);
            line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
            return true;
        }
        scanned = ch.pending.size();
        if (scanned > kReplyLimitBytes) return false;
        DWORD moved = 0;
        if (!pipeIo(ch, false, ch.chunk.data(), static_cast<DWORD>(ch.chunk.size()), moved,
                    budget) ||
            moved == 0) {
            return false;
        }
        ch.pending.append(ch.chunk.data(), moved);
    }
}

bool writeLine(HelperChannel& ch, const std::string& request, const IoBudget& budget) {
    std::string out = request;
    out.push_back('\n');
    DWORD moved = 0;
    return pipeIo(ch, true, out.data(), static_cast<DWORD>(out.size()), moved, budget) &&
           moved == out.size();
}

bool unplugAnswered(const std::string& response) {
    Json j;
    return jsonParse(response, j) && jsonBool(j, "ok");
}

bool roundTrip(HelperChannel& ch, const std::string& request, std::string& response, DWORD waitMs,
               HANDLE cancel) {
    if (ch.pipe == INVALID_HANDLE_VALUE) return false;
    const IoBudget budget = budgetFor(waitMs, cancel);
    std::string out = request;
    out.push_back('\n');
    ch.pending.clear();
    DWORD moved = 0;
    if (!pipeIo(ch, true, out.data(), static_cast<DWORD>(out.size()), moved, budget) ||
        moved != out.size()) {
        return false;
    }
    return readLine(ch, response, budget);
}

std::string helloRequest() {
    return "{\"op\":\"hello\",\"protocol\":" + std::to_string(HELPER_PROTOCOL_OFFERED) + "}";
}

int32_t settledProtocol(const Json& hello) {
    int64_t answered = 0;
    const bool named = jsonTryI64(hello, "protocol", answered);
    return settledHelperProtocol(named, answered);
}

bool handshake(HelperChannel& ch, DWORD waitMs, HANDLE cancel) {
    std::string response;
    if (!roundTrip(ch, helloRequest(), response, waitMs, cancel)) return false;
    Json j;
    if (!jsonParse(response, j) || !jsonBool(j, "ok")) return false;
    ch.protocol = settledProtocol(j);
    if (ch.protocol < HELPER_PROTOCOL_DEVICE_WATCH) {
        logMsg(LogLevel::WARN, "hidmaestro",
               "The HIDMaestro helper predates the device watch, so Satellite cannot tell when "
               "Windows loses one of its controllers");
    }
    return true;
}

bool pauseUnlessCancelled(HANDLE cancel, DWORD ms) {
    if (cancel == nullptr) {
        Sleep(ms);
        return true;
    }
    return WaitForSingleObject(cancel, ms) == WAIT_TIMEOUT;
}

HANDLE openBrokerPipe(HANDLE cancel) {
    blockingRegion("helper.pipe");
    if (!helperBinaryPresent() || !brokerServiceRegistered()) return INVALID_HANDLE_VALUE;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    const ULONGLONG deadline = GetTickCount64() + HELPER_PIPE_WAIT_MS;
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
            if (!pauseUnlessCancelled(cancel, 250)) return INVALID_HANDLE_VALUE;
        } else {
            return INVALID_HANDLE_VALUE;
        }
        if (GetTickCount64() >= deadline) return INVALID_HANDLE_VALUE;
    }

    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid == 0 ||
        serverPid != brokerServicePid(/*tryStart=*/false)) {
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
    }
    return pipe;
}

} // namespace

HelperClient::HelperClient()
    : cancel_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      connectDone_(CreateEventW(nullptr, TRUE, TRUE, nullptr)) {}

HelperClient::~HelperClient() {
    HelperClient* self = this;
    g_connectOnInstall.compare_exchange_strong(self, nullptr);
    shutdown();
    CloseHandle(cancel_);
    CloseHandle(connectDone_);
}

bool HelperClient::installed() const {
#ifdef SATELLITE_BUILD_TESTS
    if (transportsActive_) return true;
#endif
    return helperBinaryPresent() && driverInstalled();
}

bool HelperClient::isReady() const {
    std::lock_guard<TrackedMutex> lk(mtx_);
    if (channel_ != nullptr) return true;
    const HelperChannel* staged = staged_.load(std::memory_order_acquire);
    return staged != nullptr && staged->alive();
}

bool HelperClient::ensureReady() {
    awaitBackgroundConnect();
    std::lock_guard<TrackedMutex> lk(mtx_);
    adoptStagedLocked();
    if (channel_ != nullptr) return true;
    return startLocked();
}

void HelperClient::shutdown() {
    SetEvent(cancel_);
    blockingRegion("thread.join");
    if (connectThread_.joinable()) connectThread_.join();
    ResetEvent(cancel_);
    std::lock_guard<TrackedMutex> lk(mtx_);
    delete staged_.exchange(nullptr, std::memory_order_acq_rel);
    stopLocked(true);
}

void HelperClient::connectBrokerInBackground() {
    if (channelUp_.load(std::memory_order_acquire)) return;
    if (staged_.load(std::memory_order_acquire) != nullptr) return;
    bool idle = false;
    if (!connecting_.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) return;
    ResetEvent(connectDone_);
    if (connectThread_.joinable()) connectThread_.join();
    connectThread_ = std::thread([this] { connectBrokerAhead(); });
}

void HelperClient::connectBrokerAhead() {
    ScopedThreadRole role(ThreadRole::Background);
    if (installed()) {
        auto channel = std::make_unique<HelperChannel>();
        if (connectBroker(*channel)) {
            delete staged_.exchange(channel.release(), std::memory_order_acq_rel);
            logMsg(LogLevel::INFO, "hidmaestro",
                   "Connected to the SatelliteHmBroker service ahead of the first plug");
        }
    }
    SetEvent(connectDone_);
    connecting_.store(false, std::memory_order_release);
}

void HelperClient::awaitBackgroundConnect() {
    if (!connecting_.load(std::memory_order_acquire)) return;
    blockingRegion("helper.pipe");
    WaitForSingleObject(connectDone_, 2 * HELPER_PIPE_WAIT_MS + 5000);
}

void HelperClient::adoptStagedLocked() {
    if (channel_ != nullptr) return;
    std::unique_ptr<HelperChannel> staged(staged_.exchange(nullptr, std::memory_order_acq_rel));
    if (staged != nullptr && staged->alive()) setChannelLocked(std::move(staged));
}

void HelperClient::setChannelLocked(std::unique_ptr<HelperChannel> channel) {
    channel_ = std::move(channel);
    if (channel_ != nullptr) generation_++;
    channelUp_.store(channel_ != nullptr, std::memory_order_release);
    liveGeneration_.store(channel_ != nullptr ? generation_ : 0, std::memory_order_release);
}

bool HelperClient::startLocked() {
    auto channel = std::make_unique<HelperChannel>();
    if (connectBroker(*channel)) {
        setChannelLocked(std::move(channel));
        logMsg(LogLevel::INFO, "hidmaestro", "Connected to the SatelliteHmBroker service");
        return true;
    }
    if (!spawnHelper(*channel)) return false;
    setChannelLocked(std::move(channel));
    logMsg(LogLevel::INFO, "hidmaestro", "Spawned an elevated helper for this session");
    return true;
}

bool HelperClient::openChannel(HelperChannel& channel, HANDLE pipe, HANDLE process, HANDLE cancel) {
    if (!channel.attach(pipe, process)) return false;
    if (handshake(channel, waitFor(HelperRequest::Hello), cancel)) return true;
    channel.close();
    return false;
}

HANDLE HelperClient::openBroker() {
#ifdef SATELLITE_BUILD_TESTS
    if (transportsActive_) return transports_.broker ? transports_.broker() : INVALID_HANDLE_VALUE;
#endif
    return openBrokerPipe(cancel_);
}

bool HelperClient::connectBroker(HelperChannel& channel) {
    HANDLE pipe = openBroker();
    if (pipe == INVALID_HANDLE_VALUE) return false;
    return openChannel(channel, pipe, nullptr, cancel_);
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
    blockingRegion("helper.pipe");
    if (!ShellExecuteExW(&sei)) return nullptr;
    return sei.hProcess;
}

// Waits for a client to connect to `pipe`, giving up when the helper exits
// (UAC declined, crash) or the connect budget runs out.
bool awaitPipeClient(HANDLE pipe, HANDLE ioEvent, HANDLE process, DWORD waitMs) {
    blockingRegion("helper.pipe");
    OVERLAPPED ov{};
    ov.hEvent = ioEvent;
    if (ConnectNamedPipe(pipe, &ov) != 0) return true;
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) return true;
    if (err != ERROR_IO_PENDING) return false;
    HANDLE waits[2] = {ioEvent, process};
    const DWORD rc = WaitForMultipleObjects(2, waits, FALSE, waitMs);
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

void closeDuplicatedHandle(uint64_t handle) {
    if (handle != 0) CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)));
}

// The helper duplicates every handle in its reply into this process, so a
// reply the plug cannot use leaves them here to close.
void closeDuplicatedHandles(ProvisionResult& out) {
    for (uint64_t* handle : {&out.inputSection, &out.inputEvent, &out.companionEvent,
                             &out.outputSection, &out.outputEvent, &out.versionSection,
                             &out.speakerSection, &out.speakerEvent, &out.micSection, &out.micEvent,
                             &out.hapticSection, &out.hapticEvent, &out.watchSection}) {
        closeDuplicatedHandle(*handle);
        *handle = 0;
    }
}

} // namespace

HANDLE HelperClient::launch(const std::wstring& pipeName) {
#ifdef SATELLITE_BUILD_TESTS
    if (transportsActive_) return transports_.launch ? transports_.launch(pipeName) : nullptr;
#endif
    return launchHelper(helperBinaryPath(), pipeName);
}

DWORD HelperClient::waitFor(HelperRequest request) const {
    switch (request) {
    case HelperRequest::Provision:
        return provisionWaitMs_.load();
    case HelperRequest::Shutdown:
        return shutdownWaitMs_.load();
    case HelperRequest::Hello:
    case HelperRequest::Deprovision:
        return pipeWaitMs_.load();
    }
    return pipeWaitMs_.load();
}

bool HelperClient::spawnHelper(HelperChannel& channel) {
#ifdef SATELLITE_BUILD_TESTS
    if (!transportsActive_ && !helperBinaryPresent()) return false;
#else
    if (!helperBinaryPresent()) return false;
#endif
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

    HANDLE process = launch(pipeName);
    if (process == nullptr) {
        CloseHandle(ioEvent);
        CloseHandle(pipe);
        return false;
    }

    const bool connected = awaitPipeClient(pipe, ioEvent, process, launchWaitMs_.load()) &&
                           pipeClientIs(pipe, process);
    CloseHandle(ioEvent);
    if (!connected) {
        TerminateProcess(process, 1);
        CloseHandle(process);
        CloseHandle(pipe);
        return false;
    }
    return openChannel(channel, pipe, process, nullptr);
}

void HelperClient::stopLocked(bool sendShutdown) {
    if (channel_ != nullptr && sendShutdown) {
        std::string response;
        requestLocked(HelperRequest::Shutdown, "{\"op\":\"shutdown\"}", response);
        blockingRegion("helper.pipe");
        if (channel_ != nullptr && channel_->process != nullptr) {
            WaitForSingleObject(channel_->process, exitWaitMs_.load());
        }
    }
    setChannelLocked(nullptr);
}

// Answers still owed to lost pads' unplugs are read first: the helper answers
// in order, so the next line after them is this request's.
bool HelperClient::requestLocked(HelperRequest request, const std::string& line,
                                 std::string& response) {
    if (channel_ == nullptr) return false;
    switch (drainOwedLocked()) {
    case OwedAnswers::Pending:
        return false;
    case OwedAnswers::Gone:
        setChannelLocked(nullptr);
        return false;
    case OwedAnswers::Read:
        break;
    }
    if (roundTrip(*channel_, line, response, waitFor(request), nullptr)) return true;
    setChannelLocked(nullptr);
    return false;
}

// Caller holds mtx_. The owed answers get HELPER_OWED_WAIT_MS together, not
// the next request's own bound, so a plug never holds the topology lock long
// behind them; each that confirms its unplug settles that serial. One still
// missing then fails the request unsent and keeps the channel, until it is
// older than any unplug's own bound, when the helper no longer answers.
HelperClient::OwedAnswers HelperClient::drainOwedLocked() {
    const IoBudget budget = budgetFor(owedWaitMs_.load(), nullptr);
    while (!channel_->owed.empty()) {
        std::string response;
        if (!readLine(*channel_, response, budget)) {
            const bool overdue = GetTickCount64() >= channel_->owed.front().due;
            return overdue || !channel_->alive() ? OwedAnswers::Gone : OwedAnswers::Pending;
        }
        const uint32_t serial = channel_->owed.front().serial;
        channel_->owed.pop_front();
        if (unplugAnswered(response)) settleLocked(serial);
    }
    return OwedAnswers::Read;
}

void HelperClient::settleLocked(uint32_t serial) {
    unconfirmed_.erase(serial);
    settled_.push_back(serial);
}

// Caller holds mtx_, after a plug on the current channel succeeded: that
// helper swept what helpers that came before it left (Session.Profiles, the
// SDK's RemoveAllVirtualControllers for exited programs, before the create).
void HelperClient::settleSweptLocked() {
    for (auto it = unconfirmed_.begin(); it != unconfirmed_.end();) {
        if (it->second >= generation_) {
            ++it;
            continue;
        }
        settled_.push_back(it->first);
        it = unconfirmed_.erase(it);
    }
}

bool HelperClient::provision(uint32_t serial, GamepadIdentity identity, bool audio,
                             ProvisionResult& out) {
    const char* profile = profileForIdentity(identity, audio);
    if (profile == nullptr) return false;
    std::lock_guard<TrackedMutex> lk(mtx_);
    adoptStagedLocked();
    if (channel_ == nullptr) return false;

    JsonOut req;
    req["op"] = "plug";
    req["serial"] = serial;
    req["profile"] = profile;
    std::string response;
    if (!requestLocked(HelperRequest::Provision, jsonDump(req), response)) return false;

    Json j;
    if (!jsonParse(response, j) || !jsonBool(j, "ok")) return false;
    settleSweptLocked();
    auto handleField = [&j](const char* key) -> uint64_t {
        int64_t v = 0;
        return jsonTryI64(j, key, v) && v > 0 ? static_cast<uint64_t>(v) : 0;
    };
    out.controllerIndex = static_cast<uint32_t>(jsonInt(j, "index", 0));
    out.transportIndex =
        static_cast<uint32_t>(jsonInt(j, "transportIndex", static_cast<int>(out.controllerIndex)));
    out.inputSection = handleField("input");
    out.inputEvent = handleField("inputEvent");
    out.companionEvent = handleField("companionEvent");
    out.outputSection = handleField("output");
    out.outputEvent = handleField("outputEvent");
    out.versionSection = handleField("version");
    out.versionReported = j.contains("version");
    uint64_t identityTag = 0;
    out.identityTag = jsonTryU64(j, "identityTag", identityTag) ? identityTag : 0;
    out.usbip = jsonBool(j, "usbip");
    out.companion = jsonBool(j, "companion");
    double timeoutScale = 1.0;
    out.timeoutScale = jsonTryNumber(j, "timeoutScale", timeoutScale) ? timeoutScale : 1.0;

    // Only a session the hello settled on the watch's protocol takes a block; a
    // handle a helper sent outside it is this process's to close.
    const bool watches = channel_->protocol >= HELPER_PROTOCOL_DEVICE_WATCH;
    const uint64_t watch = handleField("watch");
    out.watchSection = watches ? watch : 0;
    out.watchReported = watches && j.contains("watch");
    if (!watches) closeDuplicatedHandle(watch);

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

    if (out.inputSection == 0 || out.inputEvent == 0) {
        closeDuplicatedHandles(out);
        return false;
    }
    pluggedOn_[serial] = generation_;
    out.channel = generation_;
    logMsg(LogLevel::INFO, "hidmaestro",
           "Serial " + std::to_string(serial) + " plugged as " + profile +
               " at HIDMaestro transport index " + std::to_string(out.transportIndex));
    return true;
}

void connectBrokerWhenDriverInstalled(HelperClient* client) {
    g_connectOnInstall.store(client, std::memory_order_release);
}

#ifdef SATELLITE_BUILD_TESTS
bool HelperClient::adoptChannelForTest(HANDLE pipe) {
    std::lock_guard<TrackedMutex> lk(mtx_);
    setChannelLocked(nullptr);
    auto channel = std::make_unique<HelperChannel>();
    if (!openChannel(*channel, pipe, nullptr, nullptr)) return false;
    setChannelLocked(std::move(channel));
    return true;
}

void HelperClient::setTransportsForTest(TestTransports transports) {
    transports_ = std::move(transports);
    transportsActive_ = true;
}

void HelperClient::setPipeWaitForTest(DWORD ms) { pipeWaitMs_.store(ms); }

void HelperClient::setProvisionWaitForTest(DWORD ms) { provisionWaitMs_.store(ms); }

void HelperClient::setLaunchWaitForTest(DWORD ms) { launchWaitMs_.store(ms); }

void HelperClient::setExitWaitForTest(DWORD ms) { exitWaitMs_.store(ms); }

void HelperClient::setLostUnplugWaitForTest(DWORD ms) { lostUnplugWaitMs_.store(ms); }

void HelperClient::setShutdownWaitForTest(DWORD ms) { shutdownWaitMs_.store(ms); }

void HelperClient::setOwedWaitForTest(DWORD ms) { owedWaitMs_.store(ms); }

bool HelperClient::waitForBackgroundConnectForTest(DWORD ms) {
    return WaitForSingleObject(connectDone_, ms) == WAIT_OBJECT_0;
}

uint64_t HelperClient::pipeReadsForTest() { return g_pipeReads.load(std::memory_order_relaxed); }

void notifyDriverInstalledForTest() { notifyDriverInstalled(); }

void setBeforeCancelForTest(std::function<void()> hook) { g_beforeCancel = std::move(hook); }
#endif

// A pad plugged on a helper that has since been replaced went with that
// helper: nothing is asked of the current one, and its channel is never let go
// for it; the current helper's first plug sweeps what its predecessor left,
// which settles the serial.
bool HelperClient::deprovision(uint32_t serial, Teardown why) {
    std::lock_guard<TrackedMutex> lk(mtx_);
    const auto on = pluggedOn_.find(serial);
    const uint64_t servedOn = on != pluggedOn_.end() ? on->second : generation_;
    if (on != pluggedOn_.end()) pluggedOn_.erase(on);
    const bool current = channel_ != nullptr && servedOn == generation_;
    const bool confirmed = current && deprovisionLocked(serial, why);
    if (confirmed) {
        unconfirmed_.erase(serial);
    } else {
        unconfirmed_[serial] = servedOn;
    }
    return confirmed;
}

bool HelperClient::deprovisionLocked(uint32_t serial, Teardown why) {
    if (channel_ == nullptr) return false;
    JsonOut req;
    req["op"] = "unplug";
    req["serial"] = serial;
    switch (why) {
    case Teardown::Requested: {
        std::string response;
        return requestLocked(HelperRequest::Deprovision, jsonDump(req), response) &&
               unplugAnswered(response);
    }
    case Teardown::DeviceLost:
        return lostUnplugLocked(serial, jsonDump(req));
    case Teardown::HelperGone:
        logMsg(LogLevel::WARN, "hidmaestro",
               "The HIDMaestro helper has stopped; Satellite lets it go, and the next plug "
               "reaches a new one");
        setChannelLocked(nullptr);
        return false;
    }
    return false;
}

// Caller holds mtx_. The answer gets one short wait, and none while an earlier
// one is owed; one that has not come is owed, read ahead of the next request's,
// and the channel stays for the helper's other pads.
bool HelperClient::lostUnplugLocked(uint32_t serial, const std::string& line) {
    HelperChannel& ch = *channel_;
    if (!writeLine(ch, line, budgetFor(waitFor(HelperRequest::Deprovision), nullptr))) {
        setChannelLocked(nullptr);
        return false;
    }
    ch.owed.push_back({serial, GetTickCount64() + pipeWaitMs_.load()});
    if (ch.owed.size() > 1) return false;
    std::string response;
    if (!readLine(ch, response, budgetFor(lostUnplugWaitMs_.load(), nullptr))) {
        if (!ch.alive()) setChannelLocked(nullptr);
        return false;
    }
    ch.owed.pop_front();
    return unplugAnswered(response);
}

uint64_t HelperClient::liveChannel() const {
    return liveGeneration_.load(std::memory_order_acquire);
}

std::vector<uint32_t> HelperClient::takeSettledSerials() {
    std::lock_guard<TrackedMutex> lk(mtx_);
    return std::exchange(settled_, {});
}

} // namespace hidmaestro
} // namespace satellite
