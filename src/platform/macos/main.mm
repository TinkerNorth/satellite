// SPDX-License-Identifier: LGPL-3.0-or-later
// macOS entry point / Composition Root; mirrors platform/windows/main.cpp.
#include "globals.h"
#include "config.h"
#include "crypto.h"
#include "tray.h"
#include "mac_hid_gamepad_adapter.h"
#include "updater_adapter.h"

#include "net/receiver.h"
#include "net/webserver.h"
#include "net/discovery.h"
#include "net/mdns_responder.h"
#include "net/pairing.h"
#include "net/session_crypto.h"

#include "adapters/client_adapter.h"
#include "adapters/log_adapter.h"
#include "adapters/audio/opus_codec.h"

#include "core/session_service.h"
#include "core/update_service.h"
#include "adapters/crash_adapter.h"
#include "core/crash_reporting.h"

namespace crash = satellite::crash;

#include <sys/stat.h>

#import <AppKit/AppKit.h>

// Cocoa delegate: clean shutdown on terminate.
@interface SatelliteAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation SatelliteAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    g_appRunning = false;
    g_httpServer.stop();
    if (g_clientServer) g_clientServer->stop();
    return NSTerminateNow;
}
@end

// Virtual gamepads need the com.apple.developer.hid.virtual.device
// entitlement (production builds). Unentitled processes run the full protocol
// stack with the historical inert-backend behavior.
static void logBackendAvailability() {
    if (MacHidGamepadAdapter::runtimeAvailable()) {
        fprintf(stderr, "[satellite] macOS virtual-DualShock-4 backend available "
                        "(IOHIDUserDevice).\n");
    } else {
        fprintf(stderr, "[satellite] macOS stub build: virtual gamepads disabled "
                        "(controller descriptors will apply as backendUnavailable).\n");
    }
}

static bool startNetwork() {
    if (netInit()) return true;
    fprintf(stderr, "Failed to initialize network subsystem\n");
    return false;
}

static bool startCrypto() {
    if (sodiumInit()) return true;
    fprintf(stderr, "Failed to initialize libsodium\n");
    return false;
}

// The persisted config, with the autostart flag read from where the desktop
// keeps it rather than from the file.
static void loadConfigSeedingAutostart() {
    g_config = loadConfig();
    g_config.autoStart = getAutoStart();
}

// Read per frame, not cached: unlike the master switch these gate the wire
// rather than the persona, so flipping one reaches a stream already playing
// instead of waiting for a replug.
static ControllerAudioPolicy audioPolicyFromConfig() {
    std::lock_guard<std::mutex> lk(g_configMtx);
    return ControllerAudioPolicy{g_config.controllerAudioMic, g_config.controllerAudioSpeaker,
                                 g_config.controllerAudioHaptics};
}

static void persistConfig() {
    std::lock_guard<std::mutex> lk(g_configMtx);
    saveConfig(g_config);
}

// Inside an .app bundle the binary lives at Contents/MacOS/; the web UI is
// staged into Contents/Resources/web by CMake. A sibling web/ directory serves
// non-bundle / dev builds.
static std::string resolveWebDir() {
    const std::string exeDir = getExeDir();
    const std::string bundled = exeDir + "/../Resources/web";
    struct stat st;
    if (stat(bundled.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return bundled;
    return exeDir + "/web";
}

// The service's threads, joined in the order they were started.
struct Workers {
    std::thread recv;
    std::thread admin;
    std::thread client;
    std::thread disc;
    std::thread mdns;
};

static Workers startWorkers(SessionService& svc, ClientAdapter& clientAdapter) {
    Workers w;
    w.recv = std::thread(receiverThread, std::ref(svc), std::ref(clientAdapter));
    w.admin = std::thread(adminHttpThread, std::ref(svc));
    w.client = std::thread(clientApiThread, std::ref(svc));
    w.disc = std::thread(discoveryThread);
    w.mdns = std::thread(mdnsResponderThread);
    return w;
}

static void joinWorkers(Workers& w) {
    w.recv.join();
    w.admin.join();
    w.client.join();
    w.disc.join();
    w.mdns.join();
}

// The AppKit run loop on the main thread, as an accessory (no Dock icon), until
// applicationShouldTerminate: has flipped the flags.
static void runApp(SatelliteAppDelegate* delegate) {
    NSApplication* app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [app setDelegate:delegate];
    addTrayIcon();
    // Reverse-pairing: a dish request raises a native notification +
    // Accept/Reject alert so the operator never needs the web UI.
    setPairRequestListener(notifyPairRequestMac);
    [app run];
    removeTrayIcon();
}

// No command line: everything is configured through the web UI and the
// config file, so the entry point takes none.
//
// Long on purpose: the composition root. Every collaborator lives on this
// stack in the order it must be constructed, and the shutdown below mirrors
// that order; a helper per step would move the lifetimes out of sight.
int main() {
    @autoreleasepool {
        logBackendAvailability();
        if (!startNetwork() || !startCrypto()) return 1;
        loadConfigSeedingAutostart();

        // As on Linux: the only crash recorder here, and still gated on the
        // operator's opt-in plus a DSN this build actually carries.
        crash::init(g_config.crashReporting, crash::databaseDirFor(configPath()));

        MacHidGamepadAdapter gamepadAdapter;
        ClientAdapter clientAdapter;
        LogAdapter logAdapter;
        // See the Windows main for why the codec is injected rather than built
        // into the core. IOHIDUserDevice has no audio endpoint to feed today,
        // but the service's audio paths are platform-neutral and stay wired
        // the same way.
        satellite::audio::OpusCodecFactory audioCodecs;
        SessionService svc(gamepadAdapter, clientAdapter, logAdapter, deriveSessionKey,
                           &audioCodecs);
        svc.setAudioPolicy(audioPolicyFromConfig);

        MacOSUpdaterAdapter updaterAdapter("TinkerNorth", "satellite");
        UpdateService updateService(updaterAdapter, logAdapter, g_config, g_configMtx);
        updateService.setPersistCallback(persistConfig);
        g_updateService = &updateService;
        g_webDir = resolveWebDir();
        updateService.start();

        Workers workers = startWorkers(svc, clientAdapter);
        SatelliteAppDelegate* delegate = [[SatelliteAppDelegate alloc] init];
        runApp(delegate);

        // applicationShouldTerminate: has already flipped the flags.
        updateService.stop();
        g_updateService = nullptr;
        joinWorkers(workers);

        svc.closeAllSessions();
        saveConfig(g_config);

        // Flush before exit; a pending envelope is lost otherwise.
        crash::shutdown();

        netShutdown();
    }
    return 0;
}
