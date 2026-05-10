#include "platform.h"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

static void SetMacOSDockIconVisible(BOOL visible) {
    NSApplicationActivationPolicy policy = visible
        ? NSApplicationActivationPolicyRegular
        : NSApplicationActivationPolicyAccessory;
    [[NSApplication sharedApplication] setActivationPolicy:policy];
}

@interface ClippMainWindowController : NSWindowController <NSWindowDelegate>
@property(nonatomic, strong) NSView* contentContainer;
@property(nonatomic, strong) NSArray<NSButton*>* pageButtons;
@property(nonatomic, strong) NSArray<NSView*>* pageViews;
- (void)showAndActivate;
- (BOOL)isWindowVisibleOrMiniaturized;
@end

@interface ClippStatusMenuController : NSObject
@property(nonatomic, strong) NSStatusItem* statusItem;
@property(nonatomic, strong) ClippMainWindowController* mainWindowController;
@end

@implementation ClippMainWindowController

- (instancetype)init {
    NSRect frame = NSMakeRect(0, 0, 880, 560);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:(NSWindowStyleMaskTitled |
                                                              NSWindowStyleMaskClosable |
                                                              NSWindowStyleMaskMiniaturizable |
                                                              NSWindowStyleMaskResizable)
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.title = @"Clipp";
    window.releasedWhenClosed = NO;
    window.minSize = NSMakeSize(720, 440);

    self = [super initWithWindow:window];
    if (self) {
        window.delegate = self;
        [self buildShell];
        [self selectPage:0];
    }
    return self;
}

- (void)buildShell {
    NSView* root = [[NSView alloc] initWithFrame:NSZeroRect];
    root.translatesAutoresizingMaskIntoConstraints = NO;
    self.window.contentView = root;

    NSVisualEffectView* sidebar = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
    sidebar.translatesAutoresizingMaskIntoConstraints = NO;
    sidebar.material = NSVisualEffectMaterialSidebar;
    sidebar.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    sidebar.state = NSVisualEffectStateActive;

    NSView* content = [[NSView alloc] initWithFrame:NSZeroRect];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    self.contentContainer = content;

    [root addSubview:sidebar];
    [root addSubview:content];

    [NSLayoutConstraint activateConstraints:@[
        [sidebar.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [sidebar.topAnchor constraintEqualToAnchor:root.topAnchor],
        [sidebar.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
        [sidebar.widthAnchor constraintEqualToConstant:180],

        [content.leadingAnchor constraintEqualToAnchor:sidebar.trailingAnchor],
        [content.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [content.topAnchor constraintEqualToAnchor:root.topAnchor],
        [content.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
    ]];

    NSStackView* menuStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    menuStack.translatesAutoresizingMaskIntoConstraints = NO;
    menuStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    menuStack.alignment = NSLayoutAttributeLeading;
    menuStack.distribution = NSStackViewDistributionFill;
    menuStack.spacing = 6;
    menuStack.edgeInsets = NSEdgeInsetsMake(16, 12, 0, 12);

    NSArray<NSString*>* titles = @[@"Clipp", @"Settings", @"Logs", @"About"];
    NSMutableArray<NSButton*>* buttons = [NSMutableArray arrayWithCapacity:titles.count];
    for (NSUInteger index = 0; index < titles.count; ++index) {
        NSButton* button = [self makeSidebarButtonWithTitle:titles[index] action:@selector(selectPageFromButton:)];
        button.tag = static_cast<NSInteger>(index);
        [buttons addObject:button];
        [menuStack addArrangedSubview:button];
    }
    self.pageButtons = buttons;

    NSStackView* actionStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    actionStack.translatesAutoresizingMaskIntoConstraints = NO;
    actionStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    actionStack.alignment = NSLayoutAttributeLeading;
    actionStack.distribution = NSStackViewDistributionFill;
    actionStack.spacing = 8;
    actionStack.edgeInsets = NSEdgeInsetsMake(0, 12, 16, 12);

    NSButton* minimizeButton = [self makeActionButtonWithTitle:@"Minimize to Menu Bar"
                                                        action:@selector(minimizeToMenuBar:)];
    NSButton* exitButton = [self makeActionButtonWithTitle:@"Exit Application"
                                                    action:@selector(exitApplication:)];
    [actionStack addArrangedSubview:minimizeButton];
    [actionStack addArrangedSubview:exitButton];

    [sidebar addSubview:menuStack];
    [sidebar addSubview:actionStack];

    [NSLayoutConstraint activateConstraints:@[
        [menuStack.leadingAnchor constraintEqualToAnchor:sidebar.leadingAnchor],
        [menuStack.trailingAnchor constraintEqualToAnchor:sidebar.trailingAnchor],
        [menuStack.topAnchor constraintEqualToAnchor:sidebar.topAnchor],
        [menuStack.bottomAnchor constraintLessThanOrEqualToAnchor:actionStack.topAnchor constant:-16],

        [actionStack.leadingAnchor constraintEqualToAnchor:sidebar.leadingAnchor],
        [actionStack.trailingAnchor constraintEqualToAnchor:sidebar.trailingAnchor],
        [actionStack.bottomAnchor constraintEqualToAnchor:sidebar.bottomAnchor],
    ]];

    self.pageViews = @[
        [self makePlaceholderPageWithTitle:@"Clipp"
                                      body:@"Clipp status, discovered peers, and sync controls will live here."],
        [self makePlaceholderPageWithTitle:@"Settings"
                                      body:@"Network, startup, and clipboard behavior settings will live here."],
        [self makePlaceholderPageWithTitle:@"Logs"
                                      body:@"Runtime logs and diagnostic output will live here."],
        [self makePlaceholderPageWithTitle:@"About"
                                      body:@"About Clipp details, version information, and credits will live here."],
    ];
}

- (NSButton*)makeSidebarButtonWithTitle:(NSString*)title action:(SEL)action {
    NSButton* button = [NSButton buttonWithTitle:title target:self action:action];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.bezelStyle = NSBezelStyleTexturedRounded;
    button.buttonType = NSButtonTypeToggle;
    button.alignment = NSTextAlignmentLeft;
    button.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    [button.widthAnchor constraintEqualToConstant:156].active = YES;
    return button;
}

- (NSButton*)makeActionButtonWithTitle:(NSString*)title action:(SEL)action {
    NSButton* button = [NSButton buttonWithTitle:title target:self action:action];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.bezelStyle = NSBezelStyleRounded;
    button.font = [NSFont systemFontOfSize:12];
    [button.widthAnchor constraintEqualToConstant:156].active = YES;
    return button;
}

- (NSView*)makePlaceholderPageWithTitle:(NSString*)title body:(NSString*)body {
    NSView* page = [[NSView alloc] initWithFrame:NSZeroRect];
    page.translatesAutoresizingMaskIntoConstraints = NO;

    NSStackView* stack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.distribution = NSStackViewDistributionGravityAreas;
    stack.spacing = 14;
    stack.edgeInsets = NSEdgeInsetsMake(28, 28, 28, 28);

    NSTextField* heading = [NSTextField labelWithString:title];
    heading.translatesAutoresizingMaskIntoConstraints = NO;
    heading.font = [NSFont systemFontOfSize:28 weight:NSFontWeightSemibold];
    heading.textColor = [NSColor labelColor];

    NSTextField* text = [NSTextField wrappingLabelWithString:body];
    text.translatesAutoresizingMaskIntoConstraints = NO;
    text.font = [NSFont systemFontOfSize:14];
    text.textColor = [NSColor secondaryLabelColor];

    [stack addArrangedSubview:heading];
    [stack addArrangedSubview:text];
    [page addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:page.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:page.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:page.topAnchor],
        [text.widthAnchor constraintLessThanOrEqualToConstant:520],
    ]];

    return page;
}

- (void)selectPageFromButton:(id)sender {
    NSButton* button = static_cast<NSButton*>(sender);
    [self selectPage:button.tag];
}

- (void)selectPage:(NSInteger)pageIndex {
    if (pageIndex < 0 || pageIndex >= static_cast<NSInteger>(self.pageViews.count)) {
        return;
    }

    for (NSButton* button in self.pageButtons) {
        button.state = button.tag == pageIndex ? NSControlStateValueOn : NSControlStateValueOff;
    }

    for (NSView* subview in self.contentContainer.subviews) {
        [subview removeFromSuperview];
    }

    NSView* page = self.pageViews[static_cast<NSUInteger>(pageIndex)];
    [self.contentContainer addSubview:page];
    [NSLayoutConstraint activateConstraints:@[
        [page.leadingAnchor constraintEqualToAnchor:self.contentContainer.leadingAnchor],
        [page.trailingAnchor constraintEqualToAnchor:self.contentContainer.trailingAnchor],
        [page.topAnchor constraintEqualToAnchor:self.contentContainer.topAnchor],
        [page.bottomAnchor constraintEqualToAnchor:self.contentContainer.bottomAnchor],
    ]];
}

- (void)showAndActivate {
    SetMacOSDockIconVisible(YES);
    if (self.window.miniaturized) {
        [self.window deminiaturize:nil];
    }
    [self showWindow:nil];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
}

- (void)minimizeToMenuBar:(id)sender {
    (void)sender;
    [self.window orderOut:nil];
    SetMacOSDockIconVisible(NO);
}

- (void)exitApplication:(id)sender {
    (void)sender;
    RequestMacOSAppShutdown();
}

- (BOOL)windowShouldClose:(id)sender {
    (void)sender;
    [self minimizeToMenuBar:nil];
    return NO;
}

- (BOOL)isWindowVisibleOrMiniaturized {
    return self.window.visible || self.window.miniaturized;
}

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

    NSMenuItem* openItem = [[NSMenuItem alloc] initWithTitle:@"Open Clipp"
                                                      action:@selector(openClipp:)
                                               keyEquivalent:@""];
    openItem.target = self;
    [menu addItem:openItem];

    [menu addItem:[NSMenuItem separatorItem]];

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

- (void)openClipp:(id)sender {
    (void)sender;

    if (self.mainWindowController == nil) {
        self.mainWindowController = [[ClippMainWindowController alloc] init];
    }

    [self.mainWindowController showAndActivate];
}

- (void)showAbout:(id)sender {
    (void)sender;

    const BOOL hasMainWindow = [self.mainWindowController isWindowVisibleOrMiniaturized];
    SetMacOSDockIconVisible(YES);
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Clipp v1.0";
    alert.informativeText = @"Secure cross-platform clipboard sync.";
    alert.alertStyle = NSAlertStyleInformational;
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    if (!hasMainWindow) {
        SetMacOSDockIconVisible(NO);
    }
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
        SetMacOSDockIconVisible(NO);

        static ClippStatusMenuController* controller = nil;
        controller = [[ClippStatusMenuController alloc] init];

        [app run];
    }
}

#endif
