#include "platform.h"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

@interface ClippStatusMenuController : NSObject
@property(nonatomic, strong) NSStatusItem* statusItem;
@end

@implementation ClippStatusMenuController

- (instancetype)init {
    self = [super init];
    if (self) {
        [self buildMenu];
    }
    return self;
}

- (void)buildMenu {
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];

    NSStatusBarButton* button = [self.statusItem button];
    NSImage* clipboardImage = [NSImage imageWithSystemSymbolName:@"doc.on.clipboard"
                                        accessibilityDescription:@"Clipp"];
    if (clipboardImage != nil) {
        button.image = clipboardImage;
    } else {
        button.title = @"📋";
    }
    button.toolTip = @"Clipp Network Sync";

    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Clipp"];

    NSMenuItem* aboutItem = [[NSMenuItem alloc] initWithTitle:@"About Clipp"
                                                       action:@selector(showAbout:)
                                                keyEquivalent:@""];
    aboutItem.target = self;
    [menu addItem:aboutItem];

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* exitItem = [[NSMenuItem alloc] initWithTitle:@"Exit Clipp"
                                                      action:@selector(exitApp:)
                                               keyEquivalent:@""];
    exitItem.target = self;
    [menu addItem:exitItem];

    self.statusItem.menu = menu;
}

- (void)showAbout:(id)sender {
    (void)sender;

    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Clipp v1.0";
    alert.informativeText = @"Secure cross-platform clipboard sync.";
    alert.alertStyle = NSAlertStyleInformational;
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
}

- (void)exitApp:(id)sender {
    (void)sender;
    RequestMacOSAppShutdown();
}

@end

static void StopMacOSAppOnMainThread() {
    NSApplication* app = [NSApplication sharedApplication];
    [app stop:nil];

    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [app postEvent:event atStart:NO];
}

void RequestMacOSAppShutdown() {
    if ([NSThread isMainThread]) {
        StopMacOSAppOnMainThread();
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        StopMacOSAppOnMainThread();
    });
}

void RunMacOSStatusMenu() {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        static ClippStatusMenuController* controller = nil;
        controller = [[ClippStatusMenuController alloc] init];

        [app run];
    }
}

#endif
