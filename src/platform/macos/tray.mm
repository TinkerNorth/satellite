// SPDX-License-Identifier: LGPL-3.0-or-later
#include "tray.h"
#include "config.h"
#include "core/update_service.h"
#include "net/pairing.h"
#include "net/pairing_service.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

@interface SatelliteTrayTarget : NSObject <NSUserNotificationCenterDelegate>
- (void)openUI:(id)sender;
- (void)donate:(id)sender;
- (void)updateAction:(id)sender;
- (void)quit:(id)sender;
- (void)rebuildMenu;
@end

static NSStatusItem* g_statusItem = nil;
static SatelliteTrayTarget* g_target = nil;

@implementation SatelliteTrayTarget

- (void)openUI:(id)sender {
    (void)sender;
    NSString* url = [NSString stringWithFormat:@"http://localhost:%d", g_config.webPort];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
}

- (void)donate:(id)sender {
    (void)sender;
    NSString* url = [NSString stringWithFormat:@"http://localhost:%d/donate", g_config.webPort];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
}

- (void)updateAction:(id)sender {
    (void)sender;
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
    (void)sender;
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
            updateTitle = [NSString stringWithFormat:@"Install Update v%s", s.info.version.c_str()];
        } else if (s.state == UpdateState::UpdateAvailable && s.info.available) {
            updateTitle =
                [NSString stringWithFormat:@"Download Update v%s…", s.info.version.c_str()];
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

// Always present: the app is an accessory item, never frontmost.
- (BOOL)userNotificationCenter:(NSUserNotificationCenter*)center
     shouldPresentNotification:(NSUserNotification*)notification {
    (void)center;
    (void)notification;
    return YES;
}

// The Accept/Reject alert shows the dish's PIN so the operator confirms it
// matches the device; that visual match is the auth (see net/pairing.h).
- (void)userNotificationCenter:(NSUserNotificationCenter*)center
       didActivateNotification:(NSUserNotification*)notification {
    [center removeDeliveredNotification:notification];
    NSString* devId = notification.userInfo[@"deviceId"];
    if (devId == nil) return;
    std::string deviceId([devId UTF8String]);

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
    [NSApp activateIgnoringOtherApps:YES];
    if ([alert runModal] == NSAlertFirstButtonReturn) {
        confirmPairing(deviceId);
    } else {
        declinePairing(deviceId);
    }
}

@end

void addTrayIcon() {
    g_statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    NSStatusBarButton* btn = [g_statusItem button];
    [btn setTitle:@"🛰"];
    [btn setToolTip:@(APP_TITLE)];

    g_target = [[SatelliteTrayTarget alloc] init];
    [g_target rebuildMenu];
    [NSUserNotificationCenter defaultUserNotificationCenter].delegate = g_target;
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
      NSUserNotification* n = [[NSUserNotification alloc] init];
      n.title = @"Pairing request";
      n.informativeText =
          [NSString stringWithFormat:@"%s (%s) wants to pair. Click to accept or reject.",
                                     name.empty() ? "A device" : name.c_str(), ip.c_str()];
      n.userInfo = @{@"deviceId" : [NSString stringWithUTF8String:devCopy.c_str()]};
      [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:n];
    });
}

void removeTrayIcon() {
    if (g_statusItem != nil) {
        [[NSStatusBar systemStatusBar] removeStatusItem:g_statusItem];
        g_statusItem = nil;
    }
    g_target = nil;
}
