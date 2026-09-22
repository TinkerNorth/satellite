// SPDX-License-Identifier: LGPL-3.0-or-later
#include "tray.h"
#include "config.h"
#include "core/update_service.h"
#include "net/pairing.h"
#include "net/pairing_service.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <cstdio>

@interface SatelliteTrayTarget : NSObject <UNUserNotificationCenterDelegate>
- (void)openUI:(id)sender;
- (void)donate:(id)sender;
- (void)updateAction:(id)sender;
- (void)quit:(id)sender;
- (void)rebuildMenu;
@end

static NSStatusItem* g_statusItem = nil;
static SatelliteTrayTarget* g_target = nil;

// Pairing notifications are keyed per device, so a repeat request from the
// same dish replaces its earlier banner instead of stacking a second one.
static NSString* pairNotificationIdentifier(const std::string& deviceId) {
    return [NSString stringWithFormat:@"com.tinkernorth.satellite.pair.%s", deviceId.c_str()];
}

// UserNotifications needs a bundle identifier behind the process and aborts
// without one (a bare binary run from outside satellite.app). The dashboard
// shows every pairing request regardless, so the notification is skipped
// rather than the daemon lost.
static UNUserNotificationCenter* notificationCenter() {
    if ([[NSBundle mainBundle] bundleIdentifier] == nil) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "[satellite] no bundle identifier: pairing requests will not "
                                 "raise notifications (use the web UI)\n");
        }
        return nil;
    }
    return [UNUserNotificationCenter currentNotificationCenter];
}

// The Accept/Reject alert shows the dish's PIN so the operator confirms it
// matches the device; that visual match is the auth (see net/pairing.h).
// Main thread only (AppKit).
static void promptForPairing(const std::string& deviceId) {
    std::string name, ip, pin;
    int secs = 0;
    // Re-snapshot: the request may have expired or been handled since delivery.
    if (!pairRequestSnapshot(deviceId, name, ip, pin, secs)) return;

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText =
        [NSString stringWithFormat:@"%s wants to pair", name.empty() ? "A device" : name.c_str()];
    alert.informativeText = [NSString
        stringWithFormat:@"From %s\n\nPIN on the device:  %s\n\nConfirm this matches the PIN shown "
                         @"on the device, then choose Accept.",
                         ip.c_str(), pin.c_str()];
    [alert addButtonWithTitle:@"Accept"];
    [alert addButtonWithTitle:@"Reject"];
    // The operator just clicked the notification, so the system grants the
    // activation and the alert comes to the front (the app is an accessory).
    [NSApp activate];
    if ([alert runModal] == NSAlertFirstButtonReturn) {
        confirmPairing(deviceId);
    } else {
        declinePairing(deviceId);
    }
}

@implementation SatelliteTrayTarget

- (void)openUI:(id)sender {
    NSString* url = [NSString stringWithFormat:@"http://localhost:%d", g_config.webPort];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
}

- (void)donate:(id)sender {
    NSString* url = [NSString stringWithFormat:@"http://localhost:%d/donate", g_config.webPort];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
}

- (void)updateAction:(id)sender {
    if (!g_updateService) return;
    UpdateStatusSnapshot s = g_updateService->snapshot();
    if (s.state == UpdateState::Downloaded) {
        g_updateService->requestInstall();
    } else if (s.state == UpdateState::UpdateAvailable &&
               s.info.installMethod == InstallMethod::SelfInstall) {
        g_updateService->requestDownload();
    } else {
        g_updateService->requestCheck(/*userInitiated=*/true);
    }
    NSString* url = [NSString stringWithFormat:@"http://localhost:%d/settings", g_config.webPort];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
    [self rebuildMenu];
}

- (void)quit:(id)sender {
    [NSApp terminate:nil];
}

- (void)rebuildMenu {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];

    NSMenuItem* openItem = [[NSMenuItem alloc] initWithTitle:@"Open Web UI"
                                                      action:@selector(openUI:)
                                               keyEquivalent:@""];
    [openItem setTarget:self];
    [menu addItem:openItem];

    NSMenuItem* donateItem = [[NSMenuItem alloc] initWithTitle:@"Donate"
                                                        action:@selector(donate:)
                                                 keyEquivalent:@""];
    [donateItem setTarget:self];
    [menu addItem:donateItem];

    [menu addItem:[NSMenuItem separatorItem]];

    // Label tracks state. Disabled while in flight.
    NSString* updateTitle = @"Check for Updates…";
    BOOL updateEnabled = (g_updateService != nullptr);
    if (g_updateService) {
        UpdateStatusSnapshot s = g_updateService->snapshot();
        if (s.state == UpdateState::Downloaded && s.info.available) {
            updateTitle = [NSString stringWithFormat:@"Install Update %s", s.info.version.c_str()];
        } else if (s.state == UpdateState::UpdateAvailable && s.info.available) {
            updateTitle =
                [NSString stringWithFormat:@"Download Update %s…", s.info.version.c_str()];
        } else if (s.state == UpdateState::Downloading || s.state == UpdateState::Verifying) {
            updateTitle = @"Downloading update…";
            updateEnabled = NO;
        } else if (s.state == UpdateState::Checking) {
            updateTitle = @"Checking for updates…";
            updateEnabled = NO;
        }
    }
    NSMenuItem* updateItem = [[NSMenuItem alloc] initWithTitle:updateTitle
                                                        action:@selector(updateAction:)
                                                 keyEquivalent:@""];
    [updateItem setTarget:self];
    [updateItem setEnabled:updateEnabled];
    [menu addItem:updateItem];

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                      action:@selector(quit:)
                                               keyEquivalent:@"q"];
    [quitItem setTarget:self];
    [menu addItem:quitItem];

    [g_statusItem setMenu:menu];
}

// Always present: the app is an accessory item, never frontmost, so this
// only runs when the alert itself is up; the banner still belongs on screen.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    completionHandler(UNNotificationPresentationOptionBanner |
                      UNNotificationPresentationOptionList);
}

// A click on the banner (the default action; the notification carries no
// buttons of its own) opens the Accept/Reject alert.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
    UNNotificationRequest* request = response.notification.request;
    [center removeDeliveredNotificationsWithIdentifiers:@[ request.identifier ]];
    NSString* devId = request.content.userInfo[@"deviceId"];
    // Acknowledged before the modal alert: the system expects the handler
    // back promptly, and the alert can run for as long as the operator takes.
    completionHandler();
    if (![response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]) return;
    if (devId == nil) return;
    std::string deviceId([devId UTF8String]);
    dispatch_async(dispatch_get_main_queue(), ^{ promptForPairing(deviceId); });
}

@end

void addTrayIcon() {
    g_statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    NSStatusBarButton* btn = [g_statusItem button];
    [btn setTitle:@"🛰"];
    [btn setToolTip:@(APP_TITLE)];

    g_target = [[SatelliteTrayTarget alloc] init];
    [g_target rebuildMenu];

    UNUserNotificationCenter* center = notificationCenter();
    if (center == nil) return;
    center.delegate = g_target;
    // Banners only: no sound, no badge. The system asks the operator once and
    // remembers; a refusal leaves the web UI as the place pairing requests show.
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                          completionHandler:^(BOOL granted, NSError* error) {
                            if (granted) return;
                            std::fprintf(stderr,
                                         "[satellite] notifications not authorized%s%s; pairing "
                                         "requests show in the web UI only\n",
                                         error != nil ? ": " : "",
                                         error != nil ? error.localizedDescription.UTF8String : "");
                          }];
}

// pairing.cpp listener. Fires on the HTTP thread, so it hops to the main queue
// before touching AppKit; the notification carries the deviceId so the click
// handler can re-snapshot and prompt.
void notifyPairRequestMac(const std::string& deviceId) {
    std::string devCopy = deviceId;
    dispatch_async(dispatch_get_main_queue(), ^{
      std::string name, ip, pin;
      int secs = 0;
      if (!pairRequestSnapshot(devCopy, name, ip, pin, secs)) return;
      UNUserNotificationCenter* center = notificationCenter();
      if (center == nil) return;
      UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
      content.title = @"Pairing request";
      content.body =
          [NSString stringWithFormat:@"%s (%s) wants to pair. Click to accept or reject.",
                                     name.empty() ? "A device" : name.c_str(), ip.c_str()];
      content.userInfo = @{@"deviceId" : [NSString stringWithUTF8String:devCopy.c_str()]};
      // No trigger: deliver now.
      UNNotificationRequest* request =
          [UNNotificationRequest requestWithIdentifier:pairNotificationIdentifier(devCopy)
                                               content:content
                                               trigger:nil];
      [center addNotificationRequest:request
               withCompletionHandler:^(NSError* error) {
                 if (error == nil) return;
                 std::fprintf(stderr, "[satellite] pairing notification not delivered: %s\n",
                              error.localizedDescription.UTF8String);
               }];
    });
}

void removeTrayIcon() {
    if (g_statusItem != nil) {
        [[NSStatusBar systemStatusBar] removeStatusItem:g_statusItem];
        g_statusItem = nil;
    }
    g_target = nil;
}
