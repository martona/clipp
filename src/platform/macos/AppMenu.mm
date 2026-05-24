#include "platform.h"

#ifdef __APPLE__

#include "Clipboard.h"
#include "Logger.h"
#include "AboutPage.h"
#include "ClippPage.h"
#include "NetworkPage.h"
#include "SettingsPage.h"
#include "TerminalLogView.h"
#include "platform/uistrings.h"

#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>
#import <dispatch/dispatch.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace {
std::atomic_bool g_pendingOpenMainWindow{ false };
std::atomic_bool g_pendingOpenNetworkPage{ false };

constexpr NSInteger kPageClipp = 0;
constexpr NSInteger kPageNetwork = 1;
constexpr NSInteger kPageSettings = 2;
constexpr NSInteger kPageLogs = 3;

void MakePageFlexible(NSView* view) {
    [view setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [view setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [view setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [view setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
}

void ConnectKeyViewSequence(NSArray<NSView*>* views) {
    const NSUInteger count = views.count;
    if (count == 0) {
        return;
    }

    for (NSUInteger index = 0; index < count; ++index) {
        views[index].nextKeyView = views[(index + 1) % count];
    }
}

ClipboardPayload MakeTextClipboardPayload(NSString* text) {
    ClipboardPayload payload{};
    payload.formatId = CF_UNICODETEXT;

    NSString* value = text != nil ? text : @"";
    NSData* data = [value dataUsingEncoding:NSUTF8StringEncoding];
    if (data != nil && data.length > 0) {
        const unsigned char* bytes = static_cast<const unsigned char*>(data.bytes);
        payload.rawData.assign(bytes, bytes + data.length);
    }
    payload.rawData.push_back('\0');

    return payload;
}
}

@class ClippStatusMenuController;
@class ClippMainWindowController;
static ClippStatusMenuController* g_statusMenuController = nil;
static ClippMainWindowController* g_logReflectorTarget = nil;
extern ClipboardActivityStore g_clipboardActivityStore;

static void LogReflectorCallback(const std::wstring& line);

static NSImage* MakeClippStatusItemImage() {
    NSImage* image = [NSImage imageNamed:@"ClippMenuBarTemplate"];
    if (image != nil) {
        image.size = NSMakeSize(18.0, 18.0);
        [image setTemplate:NO];
        return image;
    }

    image = [NSImage imageWithSystemSymbolName:@"doc.on.clipboard" accessibilityDescription:CLP_NS(CLP_UI_APP_NAME)];
    if (image != nil) {
        [image setTemplate:YES];
    }
    return image;
}

@interface ClippSidebarButtonCell : NSButtonCell
@end

@implementation ClippSidebarButtonCell

- (NSRect)titleRectForBounds:(NSRect)rect {
    NSRect titleRect = [super titleRectForBounds:rect];
    titleRect.origin.x += 9;
    titleRect.size.width = (std::max)(static_cast<CGFloat>(0), titleRect.size.width - 12);
    return titleRect;
}

@end

static void SetMacOSDockIconVisible(BOOL visible) {
    NSApplicationActivationPolicy policy = visible
        ? NSApplicationActivationPolicyRegular
        : NSApplicationActivationPolicyAccessory;
    [[NSApplication sharedApplication] setActivationPolicy:policy];
}

static NSString* MacOSServiceErrorDescription(NSError* error) {
    if (error == nil) {
        return @"";
    }
    NSString* reason = error.localizedFailureReason;
    if (reason.length > 0) {
        return [NSString stringWithFormat:@"%@ (%@)", error.localizedDescription, reason];
    }
    return error.localizedDescription;
}

bool RegisterClippAutoStart() {
    SMAppService* service = SMAppService.mainAppService;
    if (service.status == SMAppServiceStatusEnabled) {
        return true;
    }
    if (service.status == SMAppServiceStatusRequiresApproval) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, "Clipp is registered for login but still requires approval in macOS Login Items.");
        return false;
    }

    NSError* error = nil;
    if (![service registerAndReturnError:&error]) {
        NSString* message = MacOSServiceErrorDescription(error);
        const char* text = message.UTF8String != nullptr ? message.UTF8String : "";
        g_logger.log(__FUNCTION__, Logger::Level::Warning, "Failed to register Clipp as a login item: %s", text);
        return false;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, "Registered Clipp as a login item.");
    return true;
}

bool UnregisterClippAutoStart() {
    SMAppService* service = SMAppService.mainAppService;
    if (service.status == SMAppServiceStatusNotRegistered) {
        return true;
    }

    NSError* error = nil;
    if (![service unregisterAndReturnError:&error]) {
        NSString* message = MacOSServiceErrorDescription(error);
        const char* text = message.UTF8String != nullptr ? message.UTF8String : "";
        g_logger.log(__FUNCTION__, Logger::Level::Warning, "Failed to unregister Clipp as a login item: %s", text);
        return false;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, "Unregistered Clipp as a login item.");
    return true;
}

@interface ClippMainWindowController : NSWindowController <NSWindowDelegate> {
@private
    std::unique_ptr<MacOSAboutPage> aboutPage_;
    std::unique_ptr<MacOSClippPage> clippPage_;
    std::unique_ptr<MacOSNetworkPage> networkPage_;
    std::unique_ptr<MacOSSettingsPage> settingsPage_;
    std::unique_ptr<MacOSTerminalLogView> terminalLogView_;
    std::mutex terminalLogViewMutex_;
    NSButton* copyLogsButton_;
    bool logReflectorRegistered_;
    NSInteger currentPageIndex_;
}
@property(nonatomic, strong) NSView* contentContainer;
@property(nonatomic, strong) NSArray<NSButton*>* pageButtons;
@property(nonatomic, strong) NSArray<NSButton*>* actionButtons;
@property(nonatomic, strong) NSArray<NSView*>* pageViews;
- (void)showAndActivate;
- (void)showAndActivatePage:(NSInteger)pageIndex;
- (void)selectPage:(NSInteger)pageIndex;
- (BOOL)isWindowVisibleOrMiniaturized;
- (void)reflectLogLine:(const std::wstring&)line;
- (void)copyLogsToClipboard:(id)sender;
- (void)updateCopyLogsButtonTitle;
- (void)updateSidebarSelectionAppearance;
- (void)updateKeyViewLoopForPage:(NSInteger)pageIndex;
- (void)activateCurrentPage;
- (void)deactivateCurrentPage;
- (void)networkKeyChanged;
@end

@interface ClippStatusMenuController : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSStatusItem* statusItem;
@property(nonatomic, strong) ClippMainWindowController* mainWindowController;
- (void)openClipp:(id)sender;
- (void)openClippPage:(NSInteger)pageIndex;
@end

void RequestMacOSShowMainWindow(bool showNetworkPage) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_statusMenuController != nil) {
            [g_statusMenuController openClippPage:(showNetworkPage ? kPageNetwork : kPageClipp)];
            return;
        }
        g_pendingOpenMainWindow.store(true);
        g_pendingOpenNetworkPage.store(showNetworkPage);
    });
}

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
    window.title = CLP_NS(CLP_UI_APP_NAME);
    window.releasedWhenClosed = NO;
    window.minSize = NSMakeSize(720, 440);

    self = [super initWithWindow:window];
    if (self) {
        window.delegate = self;
        logReflectorRegistered_ = false;
        currentPageIndex_ = -1;
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

    NSArray<NSString*>* titles = @[
        CLP_NS(CLP_UI_APP_NAME),
        CLP_NS(CLP_UI_NETWORK),
        CLP_NS(CLP_UI_SETTINGS),
        CLP_NS(CLP_UI_LOGS),
        CLP_NS(CLP_UI_ABOUT)
    ];
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
    NSButton* exitButton = [self makeActionButtonWithTitle:CLP_NS(CLP_UI_EXIT_CLIPP)
                                                    action:@selector(exitApplication:)];
    [actionStack addArrangedSubview:minimizeButton];
    [actionStack addArrangedSubview:exitButton];
    self.actionButtons = @[minimizeButton, exitButton];

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

    __unsafe_unretained ClippMainWindowController* controller = self;
    clippPage_ = std::make_unique<MacOSClippPage>(g_clipboardActivityStore, [controller]() {
        [controller selectPage:kPageNetwork];
    });
    networkPage_ = std::make_unique<MacOSNetworkPage>([controller]() {
        [controller updateKeyViewLoopForPage:kPageNetwork];
    }, [controller]() {
        [controller networkKeyChanged];
    });
    settingsPage_ = std::make_unique<MacOSSettingsPage>();
    aboutPage_ = std::make_unique<MacOSAboutPage>();
    self.pageViews = @[
        clippPage_->View(),
        networkPage_->View(),
        settingsPage_->View(),
        [self makeLogsPage],
        aboutPage_->View(),
    ];
    self.window.autorecalculatesKeyViewLoop = NO;
}

- (NSButton*)makeSidebarButtonWithTitle:(NSString*)title action:(SEL)action {
    NSButton* button = [NSButton buttonWithTitle:title target:self action:action];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.cell = [[ClippSidebarButtonCell alloc] initTextCell:title];
    button.target = self;
    button.action = action;
    button.bezelStyle = NSBezelStyleRegularSquare;
    button.bordered = NO;
    button.buttonType = NSButtonTypeToggle;
    button.alignment = NSTextAlignmentLeft;
    button.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    button.wantsLayer = YES;
    button.layer.cornerRadius = 6;
    button.layer.masksToBounds = YES;
    [button.widthAnchor constraintEqualToConstant:156].active = YES;
    [button.heightAnchor constraintEqualToConstant:30].active = YES;
    return button;
}

- (NSAttributedString*)sidebarTitleForButton:(NSButton*)button selected:(BOOL)selected active:(BOOL)active {
    NSMutableParagraphStyle* paragraph = [[NSMutableParagraphStyle alloc] init];
    paragraph.alignment = NSTextAlignmentLeft;

    NSColor* textColor = [NSColor labelColor];
    if (selected && active) {
        textColor = [NSColor selectedMenuItemTextColor];
    }

    return [[NSAttributedString alloc] initWithString:button.title
                                          attributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:13 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: textColor,
        NSParagraphStyleAttributeName: paragraph,
    }];
}

- (void)updateSidebarSelectionAppearance {
    const BOOL active = self.window.keyWindow;
    for (NSButton* button in self.pageButtons) {
        const BOOL selected = button.state == NSControlStateValueOn;
        NSColor* fillColor = [NSColor clearColor];
        if (selected) {
            fillColor = active
                ? [NSColor selectedContentBackgroundColor]
                : [NSColor unemphasizedSelectedContentBackgroundColor];
        }

        button.layer.backgroundColor = fillColor.CGColor;
        button.attributedTitle = [self sidebarTitleForButton:button selected:selected active:active];
    }
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

- (NSView*)makeLogsPage {
    NSView* page = [[NSView alloc] initWithFrame:NSZeroRect];
    page.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField* heading = [NSTextField labelWithString:CLP_NS(CLP_UI_LOGS)];
    heading.translatesAutoresizingMaskIntoConstraints = NO;
    heading.font = [NSFont systemFontOfSize:28 weight:NSFontWeightSemibold];
    heading.textColor = [NSColor labelColor];

    NSTextField* intro = [NSTextField wrappingLabelWithString:CLP_NS(CLP_UI_LIVE_DIAGNOSTIC_OUTPUT)];
    intro.translatesAutoresizingMaskIntoConstraints = NO;
    intro.font = [NSFont systemFontOfSize:14];
    intro.textColor = [NSColor secondaryLabelColor];

    copyLogsButton_ = [NSButton buttonWithTitle:@""
                                         target:self
                                         action:@selector(copyLogsToClipboard:)];
    copyLogsButton_.translatesAutoresizingMaskIntoConstraints = NO;
    copyLogsButton_.bezelStyle = NSBezelStyleRounded;
    [copyLogsButton_ setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [copyLogsButton_ setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    terminalLogView_ = std::make_unique<MacOSTerminalLogView>();
    NSScrollView* logView = terminalLogView_->View();
    logView.translatesAutoresizingMaskIntoConstraints = NO;

    [page addSubview:heading];
    [page addSubview:intro];
    [page addSubview:copyLogsButton_];
    [page addSubview:logView];

    [NSLayoutConstraint activateConstraints:@[
        [heading.leadingAnchor constraintEqualToAnchor:page.leadingAnchor constant:28],
        [heading.trailingAnchor constraintEqualToAnchor:page.trailingAnchor constant:-28],
        [heading.topAnchor constraintEqualToAnchor:page.topAnchor constant:28],

        [intro.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [intro.trailingAnchor constraintEqualToAnchor:copyLogsButton_.leadingAnchor constant:-12],
        [intro.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:8],

        [copyLogsButton_.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [copyLogsButton_.centerYAnchor constraintEqualToAnchor:intro.centerYAnchor],

        [logView.leadingAnchor constraintEqualToAnchor:page.leadingAnchor constant:28],
        [logView.trailingAnchor constraintEqualToAnchor:page.trailingAnchor constant:-28],
        [logView.topAnchor constraintEqualToAnchor:intro.bottomAnchor constant:16],
        [logView.bottomAnchor constraintEqualToAnchor:page.bottomAnchor constant:-28],
    ]];

    [self updateCopyLogsButtonTitle];

    return page;
}

- (void)beginLogReflection {
    if (logReflectorRegistered_) {
        return;
    }

    g_logReflectorTarget = self;
    const auto logHistory = g_logger.AddLogReflector(LogReflectorCallback);
    {
        std::lock_guard<std::mutex> lock(terminalLogViewMutex_);
        if (terminalLogView_) {
            terminalLogView_->SetAnsiLogText(logHistory);
        }
    }
    [self updateCopyLogsButtonTitle];
    logReflectorRegistered_ = true;
}

- (void)endLogReflection {
    if (!logReflectorRegistered_) {
        return;
    }

    g_logger.RemoveLogReflector(LogReflectorCallback);
    if (g_logReflectorTarget == self) {
        g_logReflectorTarget = nil;
    }
    logReflectorRegistered_ = false;
}

- (void)reflectLogLine:(const std::wstring&)line {
    auto lineCopy = std::make_shared<std::wstring>(line);
    dispatch_async(dispatch_get_main_queue(), ^{
        {
            std::lock_guard<std::mutex> lock(terminalLogViewMutex_);
            if (logReflectorRegistered_ && terminalLogView_) {
                terminalLogView_->AppendAnsiLogText(*lineCopy);
            }
        }
        [self updateCopyLogsButtonTitle];
    });
}

- (void)copyLogsToClipboard:(id)sender {
    (void)sender;

    NSString* text = @"";
    {
        std::lock_guard<std::mutex> lock(terminalLogViewMutex_);
        if (terminalLogView_) {
            text = [NSString stringWithString:terminalLogView_->PlainText()];
        }
    }

    ClipboardPayload payload = MakeTextClipboardPayload(text);
    SetClipboardData(payload, false);
}

- (void)updateCopyLogsButtonTitle {
    if (copyLogsButton_ == nil) {
        return;
    }

    unsigned int lineCount = 0;
    {
        std::lock_guard<std::mutex> lock(terminalLogViewMutex_);
        if (terminalLogView_) {
            lineCount = terminalLogView_->LineCount();
        }
    }

    copyLogsButton_.title = [NSString stringWithFormat:CLP_NS(CLP_UI_COPY_LOG_LINES_FORMAT), static_cast<int>(lineCount)];
}

- (void)selectPageFromButton:(id)sender {
    NSButton* button = static_cast<NSButton*>(sender);
    [self selectPage:button.tag];
}

- (void)selectPage:(NSInteger)pageIndex {
    if (pageIndex < 0 || pageIndex >= static_cast<NSInteger>(self.pageViews.count)) {
        return;
    }

    const BOOL preserveWindowFrame = self.window.visible && !self.window.miniaturized;
    const NSRect windowFrame = self.window.frame;
    const NSInteger previousPageIndex = currentPageIndex_;

    if (previousPageIndex == kPageClipp && pageIndex != kPageClipp && clippPage_) {
        clippPage_->OnHidden();
    }
    if (previousPageIndex == kPageNetwork && pageIndex != kPageNetwork && networkPage_) {
        networkPage_->OnHidden();
    }
    if (previousPageIndex == kPageLogs && pageIndex != kPageLogs) {
        [self endLogReflection];
    }

    for (NSButton* button in self.pageButtons) {
        button.state = button.tag == pageIndex ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [self updateSidebarSelectionAppearance];

    for (NSView* subview in self.contentContainer.subviews) {
        [subview removeFromSuperview];
    }

    NSView* page = self.pageViews[static_cast<NSUInteger>(pageIndex)];
    MakePageFlexible(page);
    [self.contentContainer addSubview:page];
    [NSLayoutConstraint activateConstraints:@[
        [page.leadingAnchor constraintEqualToAnchor:self.contentContainer.leadingAnchor],
        [page.trailingAnchor constraintEqualToAnchor:self.contentContainer.trailingAnchor],
        [page.topAnchor constraintEqualToAnchor:self.contentContainer.topAnchor],
        [page.bottomAnchor constraintEqualToAnchor:self.contentContainer.bottomAnchor],
    ]];

    if (pageIndex == kPageSettings && settingsPage_) {
        settingsPage_->OnShown();
    }
    if (pageIndex == kPageNetwork && networkPage_) {
        networkPage_->OnShown();
    }
    if (pageIndex == kPageClipp && clippPage_) {
        clippPage_->OnShown();
    }

    currentPageIndex_ = pageIndex;
    [self updateKeyViewLoopForPage:pageIndex];

    if (pageIndex == kPageLogs) {
        [self beginLogReflection];
    }

    if (preserveWindowFrame) {
        [self.window.contentView layoutSubtreeIfNeeded];
        if (!NSEqualRects(self.window.frame, windowFrame)) {
            [self.window setFrame:windowFrame display:YES animate:NO];
        }
    }
}

- (void)updateKeyViewLoopForPage:(NSInteger)pageIndex {
    NSMutableArray<NSView*>* keyViews = [NSMutableArray arrayWithCapacity:self.pageButtons.count + self.actionButtons.count];
    [keyViews addObjectsFromArray:self.pageButtons];
    [keyViews addObjectsFromArray:self.actionButtons];
    ConnectKeyViewSequence(keyViews);

    if (pageIndex == kPageClipp && clippPage_ != nullptr && self.pageButtons.count >= 2) {
        NSView* clippButton = self.pageButtons[kPageClipp];
        NSView* nextAfterClipp = self.pageButtons[kPageNetwork];
        NSView* firstKeyView = clippPage_->FirstKeyView();
        clippButton.nextKeyView = firstKeyView != nil ? firstKeyView : nextAfterClipp;
        clippPage_->ConnectKeyViewLoop(nextAfterClipp);
        return;
    }

    if (pageIndex == kPageNetwork && networkPage_ != nullptr && self.pageButtons.count > kPageSettings) {
        NSView* networkButton = self.pageButtons[kPageNetwork];
        NSView* nextAfterNetwork = self.pageButtons[kPageSettings];
        networkButton.nextKeyView = networkPage_->FirstKeyView();
        networkPage_->ConnectKeyViewLoop(nextAfterNetwork);
        return;
    }

    if (pageIndex == kPageSettings && settingsPage_ != nullptr && self.pageButtons.count > kPageSettings) {
        NSView* settingsButton = self.pageButtons[kPageSettings];
        NSView* nextAfterSettings = self.pageButtons.count > kPageLogs
            ? self.pageButtons[kPageLogs]
            : (self.actionButtons.count > 0 ? self.actionButtons[0] : self.pageButtons[0]);

        settingsButton.nextKeyView = settingsPage_->FirstKeyView();
        settingsPage_->ConnectKeyViewLoop(nextAfterSettings);
    }
}

- (void)activateCurrentPage {
    if (currentPageIndex_ == kPageClipp && clippPage_) {
        clippPage_->OnShown();
    } else if (currentPageIndex_ == kPageNetwork && networkPage_) {
        networkPage_->OnShown();
    } else if (currentPageIndex_ == kPageSettings && settingsPage_) {
        settingsPage_->OnShown();
    } else if (currentPageIndex_ == kPageLogs) {
        [self beginLogReflection];
    }
}

- (void)deactivateCurrentPage {
    if (currentPageIndex_ == kPageClipp && clippPage_) {
        clippPage_->OnHidden();
    } else if (currentPageIndex_ == kPageNetwork && networkPage_) {
        networkPage_->OnHidden();
    } else if (currentPageIndex_ == kPageLogs) {
        [self endLogReflection];
    }
}

- (void)networkKeyChanged {
    if (clippPage_) {
        clippPage_->OnNetworkKeyChanged();
    }
}

- (void)showAndActivate {
    [self showAndActivatePage:currentPageIndex_ >= 0 ? currentPageIndex_ : kPageClipp];
}

- (void)showAndActivatePage:(NSInteger)pageIndex {
    SetMacOSDockIconVisible(YES);
    if (self.window.miniaturized) {
        [self.window deminiaturize:nil];
    }
    [self selectPage:pageIndex];
    [self showWindow:nil];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
    [self.window makeFirstResponder:nil];
    [self activateCurrentPage];
}

- (void)minimizeToMenuBar:(id)sender {
    (void)sender;
    [self deactivateCurrentPage];
    [self.window orderOut:nil];
    SetMacOSDockIconVisible(NO);
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    [self updateSidebarSelectionAppearance];
}

- (void)windowDidResignKey:(NSNotification*)notification {
    (void)notification;
    [self updateSidebarSelectionAppearance];
}

- (void)windowDidMiniaturize:(NSNotification*)notification {
    (void)notification;
    [self deactivateCurrentPage];
}

- (void)windowDidDeminiaturize:(NSNotification*)notification {
    (void)notification;
    [self activateCurrentPage];
}

- (void)exitApplication:(id)sender {
    (void)sender;
    [self deactivateCurrentPage];
    if (clippPage_) {
        clippPage_->OnDestroy();
    }
    if (networkPage_) {
        networkPage_->OnDestroy();
    }
    RequestMacOSAppShutdown(true);
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

static void LogReflectorCallback(const std::wstring& line) {
    if (g_logReflectorTarget != nil) {
        [g_logReflectorTarget reflectLogLine:line];
    }
}

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
    NSImage* clipboardImage = MakeClippStatusItemImage();
    if (clipboardImage != nil) {
        button.image = clipboardImage;
        button.imagePosition = NSImageOnly;
        button.imageScaling = NSImageScaleProportionallyDown;
    } else {
        button.title = @"📋";
    }
    button.toolTip = CLP_NS(CLP_UI_STATUS_TOOLTIP);

    NSMenu* menu = [[NSMenu alloc] initWithTitle:CLP_NS(CLP_UI_APP_NAME)];

    NSMenuItem* openItem = [[NSMenuItem alloc] initWithTitle:CLP_NS(CLP_UI_OPEN_CLIPP)
                                                      action:@selector(openClipp:)
                                               keyEquivalent:@""];
    openItem.target = self;
    [menu addItem:openItem];

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* aboutItem = [[NSMenuItem alloc] initWithTitle:CLP_NS(CLP_UI_ABOUT_CLIPP)
                                                       action:@selector(showAbout:)
                                                keyEquivalent:@""];
    aboutItem.target = self;
    [menu addItem:aboutItem];

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* exitItem = [[NSMenuItem alloc] initWithTitle:CLP_NS(CLP_UI_EXIT_CLIPP)
                                                      action:@selector(exitApp:)
                                               keyEquivalent:@""];
    exitItem.target = self;
    [menu addItem:exitItem];

    self.statusItem.menu = menu;
}

- (void)openClipp:(id)sender {
    (void)sender;
    [self openClippPage:kPageClipp];
}

- (void)openClippPage:(NSInteger)pageIndex {
    if (self.mainWindowController == nil) {
        self.mainWindowController = [[ClippMainWindowController alloc] init];
    }

    [self.mainWindowController showAndActivatePage:pageIndex];
}

- (void)showAbout:(id)sender {
    (void)sender;

    const BOOL hasMainWindow = [self.mainWindowController isWindowVisibleOrMiniaturized];
    SetMacOSDockIconVisible(YES);
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];

    NSAlert* alert = [[NSAlert alloc] init];
    NSImage* icon = [NSImage imageNamed:@"Clipp"];
    if (icon != nil) {
        alert.icon = icon;
    }
    alert.messageText = CLP_NS(CLP_UI_ABOUT_CLIPP);
    alert.informativeText =
        CLP_NS(CLP_UI_ABOUT_TITLE) @"\n"
        CLP_NS(CLP_UI_TAGLINE) @"\n\n"
        CLP_NS(CLP_UI_COPYRIGHT) @"\n"
        CLP_NS(CLP_UI_MIT_LICENSE) @"\n\n"
        @"Uses open source libraries including libsodium, lodepng, xxHash, and Zstandard.";
    alert.alertStyle = NSAlertStyleInformational;
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    if (!hasMainWindow) {
        SetMacOSDockIconVisible(NO);
    }
}

- (void)exitApp:(id)sender {
    (void)sender;
    RequestMacOSAppShutdown(true);
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)flag {
    (void)sender;
    (void)flag;
    [self openClipp:nil];
    return YES;
}

@end

static void StopMacOSAppOnMainThread(bool unregisterAutoStart) {
    if (unregisterAutoStart) {
        UnregisterClippAutoStart();
    }

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

void RequestMacOSAppShutdown(bool unregisterAutoStart) {
    if ([NSThread isMainThread]) {
        StopMacOSAppOnMainThread(unregisterAutoStart);
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        StopMacOSAppOnMainThread(unregisterAutoStart);
    });
}

void RunMacOSStatusMenu(bool showNetworkPageOnStartup) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        SetMacOSDockIconVisible(NO);

        static ClippStatusMenuController* controller = nil;
        controller = [[ClippStatusMenuController alloc] init];
        g_statusMenuController = controller;
        app.delegate = controller;
        if (showNetworkPageOnStartup) {
            g_pendingOpenMainWindow.exchange(false);
            g_pendingOpenNetworkPage.exchange(false);
            [controller openClippPage:kPageNetwork];
        } else if (g_pendingOpenMainWindow.exchange(false)) {
            const bool showNetworkPage = g_pendingOpenNetworkPage.exchange(false);
            [controller openClippPage:(showNetworkPage ? kPageNetwork : kPageClipp)];
        }

        [app run];
    }
}

#endif
