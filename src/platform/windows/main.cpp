// SPDX-License-Identifier: LGPL-3.0-or-later
#include "globals.h"
#include "config.h"
#include "crypto.h"
#include "net/receiver.h"
#include "net/webserver.h"
#include "net/discovery.h"
#include "net/mdns_responder.h"
#include "net/pairing.h"
#include "tray.h"
#include "toast.h"
#include "app_lifecycle.h"
#include "shell_integration.h"

// Adapters (outbound ports)
#include "vigem_adapter.h"
#include "updater_adapter.h"
#include "adapters/client_adapter.h"
#include "adapters/log_adapter.h"

// Domain services
#include "core/session_service.h"
#include "core/update_service.h"

#include <objbase.h>
#include <commctrl.h> // TaskDialogIndirect

#include <atomic>
#include <chrono>
#include <thread>

namespace {

// TaskDialogIndirect instead of MessageBox: modern styling, icon, hyperlinks.
void showFatalError(const wchar_t* title, const wchar_t* heading, const wchar_t* details) {
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.dwFlags = TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszWindowTitle = title;
    cfg.pszMainInstruction = heading;
    cfg.pszContent = details;
    // IDI_ERROR is MAKEINTRESOURCE-wrapped; LoadIconW's A/W split keys off the pointer.
    cfg.hMainIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_ERROR));
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

// Refresh the tray tooltip every ~2s. Cheap: updateTrayTooltip skips the
// NIM_MODIFY round-trip when the composed string is unchanged.
void tooltipTickerThread() {
    using namespace std::chrono_literals;
    while (g_appRunning.load(std::memory_order_relaxed)) {
        updateTrayTooltip();
        std::this_thread::sleep_for(2s);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    // Pre-init hardening — must run before any LoadLibrary / file I/O.
    lifecycle::hardenDllSearchPath();
    lifecycle::applyRuntimeMitigations();
    lifecycle::installCrashHandler();
    lifecycle::registerForRestart();

    // Protocol activation: a toast button launched us with a `satellite-pair:`
    // URI. Forward it to the already-running instance (which holds the pairing
    // registry) and exit — a deep link must never start a second copy.
    {
        std::string cmd = lpCmdLine != nullptr ? lpCmdLine : "";
        auto at = cmd.find("satellite-pair:");
        if (at != std::string::npos) {
            std::string uri = cmd.substr(at);
            while (!uri.empty() && (uri.back() == '"' || uri.back() == ' ')) uri.pop_back();
            HWND running = FindWindowW(L"ControllerForwardTray", nullptr);
            if (running != nullptr) {
                COPYDATASTRUCT cds{};
                cds.dwData = PAIR_URI_COPYDATA;
                cds.cbData = static_cast<DWORD>(uri.size() + 1);
                cds.lpData = const_cast<char*>(uri.c_str());
                SendMessageW(running, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
            }
            return 0;
        }
    }

    // Refuse a second copy; tap the existing instance's window so it can flash
    // a balloon, then exit (double-launch otherwise fights over ports).
    if (!lifecycle::acquireSingleInstance(APP_TITLE)) return 0;

    // AppUserModelID must be set before any HWND/tray icon, or taskbar/toast/
    // jump-list grouping falls back to "satellite.exe". COM apartment is
    // required by the jump-list registration below.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    shell_integration::registerAppUserModelID();

    // Needed for TaskDialogIndirect; done early so a failed libsodium init can
    // use the modern dialog rather than MessageBox.
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS); // low-latency input forwarding
    timeBeginPeriod(1); // default 15.6ms timer resolution would distort scheduling

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa); // httplib requires Winsock up globally

    if (!sodiumInit()) {
        showFatalError(L"Satellite", L"Cryptography library failed to initialize",
                       L"libsodium could not start. Satellite cannot run without it.\n\n"
                       L"Try reinstalling Satellite; if the problem persists, file an issue at "
                       L"https://github.com/TinkerNorth/satellite/issues.");
        return 1;
    }

    // On first run (no JSON yet) seed autostart from the registry so the
    // installer's task selection survives; the JSON is authoritative after that.
    bool firstRun;
    {
        std::ifstream probe(configPath());
        firstRun = !probe.is_open();
    }
    g_config = loadConfig();
    if (firstRun) g_config.autoStart = getAutoStart();

    // Constructive only: writes HKCU\Run when autostart is on, never deletes,
    // so a portable build can't wipe an installed copy's Run entry.
    lifecycle::reconcileAutoStart();

    // Mirror logMsg() to a rolling file; the in-memory ring alone evaporates
    // before the user can report a crash.
    lifecycle::startFileLogger();

    // Composition root: wire adapters → service.
    ViGEmAdapter vigemAdapter;
    ClientAdapter clientAdapter;
    LogAdapter logAdapter;
    SessionService svc(vigemAdapter, clientAdapter, logAdapter);

    // OTA updater. Owner/repo are baked in (forking means changing this line).
    // The persist callback runs under g_configMtx so saveConfig sees a consistent struct.
    WindowsUpdaterAdapter updaterAdapter("TinkerNorth", "satellite");
    UpdateService updateService(updaterAdapter, logAdapter, g_config, g_configMtx);
    updateService.setPersistCallback([] {
        std::lock_guard<std::mutex> lk(g_configMtx);
        saveConfig(g_config);
    });
    // Toast once when an update first appears. Edge-triggered on the
    // →UpdateAvailable transition so an open settings page doesn't spam.
    {
        // `static` is load-bearing: the callback captures this by reference and
        // outlives the block (fires from the updater worker until stop() at
        // shutdown); an automatic local here would be a use-after-free.
        static std::atomic<bool> toastedAvailable{false};
        updateService.setStatusCallback([](const UpdateStatusSnapshot& snap) {
            if (snap.state == UpdateState::UpdateAvailable && snap.info.available) {
                if (!toastedAvailable.exchange(true)) {
                    shell_integration::showToast(std::string("Satellite update ready"),
                                                 std::string("Version ") + snap.info.version +
                                                     " is available. Click to install.");
                }
            } else if (snap.state == UpdateState::Idle || snap.state == UpdateState::UpToDate) {
                toastedAvailable.store(false); // re-arm so the next UpdateAvailable toasts again
            }
            updateTrayTooltip();
        });
    }
    g_updateService = &updateService;

    // Hidden message-only window; class name is the literal Explorer/another
    // instance uses to find us (see acquireSingleInstance / protocol forward).
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ControllerForwardTray";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Satellite", 0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, hInst, nullptr);

    addTrayIcon(g_hwnd);

    // Reverse-pairing: when a dish submits a request, raise a native toast +
    // Accept/Reject dialog so the operator never has to open the web UI.
    setPairRequestListener(notifyPairRequestWindows);

    // satellite-pair: scheme lets the toast's Accept/Reject route back here.
    registerPairProtocol();

    // Jump list must come after AUMID + COM init; CommitList is idempotent.
    shell_integration::refreshJumpList();

    g_webDir = getExeDir() + "\\web";

    updateService.start(); // spawns worker + timer threads internally

    std::thread recvTh(receiverThread, std::ref(svc), std::ref(clientAdapter));
    std::thread adminTh(adminHttpThread, std::ref(svc));
    std::thread clientTh(clientApiThread, std::ref(svc));
    std::thread discTh(discoveryThread);
    std::thread mdnsTh(mdnsResponderThread);
    std::thread tooltipTh(tooltipTickerThread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_appRunning = false;
    g_httpServer.stop();
    if (g_clientServer) g_clientServer->stop();

    // Stop the updater before joining the http thread so its SSE-broadcast
    // callback can't fire into a torn-down server.
    updateService.stop();
    g_updateService = nullptr;

    recvTh.join();
    adminTh.join();
    clientTh.join();
    discTh.join();
    mdnsTh.join();
    tooltipTh.join();

    svc.closeAllSessions();

    removeTrayIcon();
    saveConfig(g_config);
    timeEndPeriod(1);
    WSACleanup();
    CoUninitialize();
    return 0;
}
