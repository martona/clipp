#include "platform.h"

#ifdef __APPLE__

#include "PopupPanel.h"

#include "ClipboardActions.h"
#include "ClipboardActivityStore.h"
#include "ClipboardFormat.h"
#include "Logger.h"
#include "PopupModel.h"
#include "PopupTextMatch.h"
#include "RegisterStore.h"
#include "RegisterWire.h"
#include "Settings.h"
#include "platform/uiClippPage.h"
#include "platform/uistrings.h"
#include "UiHelpers.h"
#include "utils.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

extern ClipboardActivityStore g_clipboardActivityStore;
extern Logger g_logger;
extern Settings g_settings;

@class ClippPopupController;

namespace {

// Same footprint as the win32 popup (dips there, points here — equal numbers
// by design). One column while only the clipboard stream exists; the
// registers column (left of it) widens the panel.
constexpr CGFloat kPopupWidthOneCol = 420.0;
constexpr CGFloat kPopupWidthTwoCol = 700.0;
constexpr CGFloat kPopupHeight = 540.0;
constexpr CGFloat kFlyoutMaxTextWidth = 360.0;
constexpr CGFloat kFlyoutMaxHeight = 520.0;
constexpr CGFloat kFlyoutGap = 8.0;

// ---- NSString-side find helpers -------------------------------------------
// The model filters on std::wstring (UTF-32 on macOS), but highlight ranges
// must be UTF-16 NSString ranges — computing on the wstring and applying to
// the NSString would misalign past any non-BMP character. So the RENDER-side
// matching runs directly on NSString UTF-16 units, mirroring the logic (and
// sharing the constants) of PopupTextMatch.h. ASCII folding over UTF-16 is
// safe: surrogate halves never equal ASCII.

unichar FoldAsciiUnichar(unichar ch) {
    return (ch >= u'A' && ch <= u'Z') ? static_cast<unichar>(ch - u'A' + u'a') : ch;
}

std::vector<NSRange> FindMatchesNS(NSString* text, NSString* needle) {
    std::vector<NSRange> matches;
    const NSUInteger textLen = text.length;
    const NSUInteger needleLen = needle.length;
    if (needleLen == 0 || needleLen > textLen) {
        return matches;
    }
    for (NSUInteger start = 0; start + needleLen <= textLen; ++start) {
        bool hit = true;
        for (NSUInteger i = 0; i < needleLen; ++i) {
            if (FoldAsciiUnichar([text characterAtIndex:start + i])
                != FoldAsciiUnichar([needle characterAtIndex:i])) {
                hit = false;
                break;
            }
        }
        if (hit) {
            matches.push_back(NSMakeRange(start, needleLen));
            if (matches.size() >= popupfind::kMaxHighlightRanges) {
                break;
            }
            start += needleLen - 1;
        }
    }
    return matches;
}

// Row line re-window: shift the line to start shortly before the first match
// so it is visible inside the single truncating line.
NSString* ReWindowRowTextNS(NSString* text, NSString* filter) {
    if (filter.length == 0) {
        return text;
    }
    const auto matches = FindMatchesNS(text, filter);
    if (!matches.empty() && matches.front().location > popupfind::kRowMatchLeadChars) {
        return [@"…" stringByAppendingString:
            [text substringFromIndex:matches.front().location - popupfind::kRowMatchLeadChars]];
    }
    return text;
}

// Flyout body: the region around the first match, clip-marked.
NSString* WindowAroundFirstMatchNS(NSString* full, NSString* filter) {
    NSUInteger begin = 0;
    const auto matches = FindMatchesNS(full, filter);
    if (!matches.empty() && matches.front().location > popupfind::kPreviewLeadChars) {
        begin = matches.front().location - popupfind::kPreviewLeadChars;
    }
    const NSUInteger len = (std::min)(static_cast<NSUInteger>(popupfind::kPreviewWindowChars),
                                      full.length - begin);
    NSString* shown = [full substringWithRange:NSMakeRange(begin, len)];
    if (begin > 0) {
        shown = [@"… " stringByAppendingString:shown];
    }
    if (begin + len < full.length) {
        shown = [shown stringByAppendingString:@" …"];
    }
    return shown;
}

// Trailing shell newlines must not force a flyout for one short line (same
// rule as popupfind::TextFitsInRow).
bool TextFitsInRowNS(NSString* full) {
    NSString* core = [full stringByTrimmingCharactersInSet:
        [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (core.length == 0) {
        return true;
    }
    return [core rangeOfString:@"\n"].location == NSNotFound
        && core.length <= popupfind::kRowFitChars;
}

// Amber find-highlight with forced dark text, same as the win32 popup.
NSAttributedString* HighlightedString(NSString* text, NSString* filter,
                                      NSFont* font, NSColor* color) {
    NSMutableParagraphStyle* paragraph = [[NSMutableParagraphStyle alloc] init];
    paragraph.lineBreakMode = NSLineBreakByTruncatingTail;
    NSMutableAttributedString* result = [[NSMutableAttributedString alloc]
        initWithString:(text != nil ? text : @"")
            attributes:@{
                NSFontAttributeName: font,
                NSForegroundColorAttributeName: color,
                NSParagraphStyleAttributeName: paragraph,
            }];
    if (filter.length > 0) {
        for (const NSRange& range : FindMatchesNS(text, filter)) {
            [result addAttributes:@{
                NSBackgroundColorAttributeName:
                    [NSColor colorWithCalibratedRed:1.0 green:0.725 blue:0.0 alpha:0.59],
                NSForegroundColorAttributeName: [NSColor blackColor],
            } range:range];
        }
    }
    return result;
}

NSString* RelativeAgeNS(uint64_t unixWallMs) {
    const uint64_t nowMs = static_cast<uint64_t>([[NSDate date] timeIntervalSince1970] * 1000.0);
    const long long secs = nowMs > unixWallMs
        ? static_cast<long long>((nowMs - unixWallMs) / 1000ull) : 0;
    if (secs < 5)      return @"just now";
    if (secs < 60)     return [NSString stringWithFormat:@"%lld seconds ago", secs];
    if (secs < 120)    return @"a minute ago";
    if (secs < 3600)   return [NSString stringWithFormat:@"%lld minutes ago", secs / 60];
    if (secs < 7200)   return @"an hour ago";
    if (secs < 86400)  return [NSString stringWithFormat:@"%lld hours ago", secs / 3600];
    if (secs < 172800) return @"yesterday";
    return [NSString stringWithFormat:@"%lld days ago", secs / 86400];
}

NSString* RelativeAgeFromTimePointNS(std::chrono::system_clock::time_point when) {
    const uint64_t ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            when.time_since_epoch()).count());
    return RelativeAgeNS(ms);
}

// Row-render info for one register, resolved once per rebuild.
struct RegisterRowInfo {
    NSString* name = nil;
    NSString* previewText = nil;   // content line: text window, kind label, or mask
    NSString* fullText = nil;      // text registers: full value for the flyout
    NSData* imageData = nil;       // BinaryHeader image stream, or nil
    bool contentRow = false;       // previewText is content: find matches + re-windows it
    bool isPrivate = false;
    uint64_t touchedWallMs = 0;    // same clock `clipp ls` shows
};

}  // namespace

// ---- panels ----------------------------------------------------------------

// The main popup: borderless nonactivating panel that CAN become key (takes
// keystrokes without activating the app). F2 and ⌘-equivalents that the field
// editor doesn't consume land here.
@interface ClippPopupMainPanel : NSPanel
@property(nonatomic, weak) ClippPopupController* controller;
@end

// Flyout / toast satellites: borderless panels that never become key, so they
// can never steal the filter field's keyboard or trip light-dismiss.
@interface ClippPopupSatellitePanel : NSPanel
@end

@implementation ClippPopupSatellitePanel
- (BOOL)canBecomeKeyWindow {
    return NO;
}
@end

// One compact list row: click selects, double-click activates, right-click
// asks the controller for the context menu.
@interface ClippPopupRowView : NSView
@property(nonatomic, weak) ClippPopupController* controller;
@property(nonatomic, assign) NSInteger group;  // 0 = Registers, 1 = History
@property(nonatomic, assign) NSInteger index;  // VISIBLE index within the group
@end

// NSSearchFieldDelegate extends NSTextFieldDelegate, so one conformance
// covers both the filter field and the inline rename editor.
@interface ClippPopupController : NSObject <NSSearchFieldDelegate> {
@public
    PopupModel model_;
    std::unordered_map<uint64_t, ClipboardActivityDisplayItem> displayCache_;
    std::map<std::string, RegisterRowInfo> registerCache_;
    std::optional<std::string> editingRegister_;
    std::size_t watcherID_;
    bool registersPresent_;
    bool flyoutUpdatePending_;
}
@property(nonatomic, strong) ClippPopupMainPanel* panel;
@property(nonatomic, strong) NSSearchField* filterField;
@property(nonatomic, strong) NSView* registerColumn;
@property(nonatomic, strong) NSTextField* registersLabel;
@property(nonatomic, strong) NSTextField* historyLabel;
@property(nonatomic, strong) NSStackView* registerRowsStack;
@property(nonatomic, strong) NSStackView* historyRowsStack;
@property(nonatomic, strong) NSScrollView* registerScroll;
@property(nonatomic, strong) NSScrollView* historyScroll;
@property(nonatomic, strong) NSButton* saveButton;
// The "Paste" toolbar button (make-current, same as Return). Historical note:
// not named copy*/paste-adjacent "copyButton" — any ARC property getter whose
// name LEADS with the word "copy" falls into the `copy` method family (owned
// +1 return) and the compiler rejects it.
@property(nonatomic, strong) NSButton* activateButton;
@property(nonatomic, strong) NSButton* renameButton;
@property(nonatomic, strong) NSButton* privateButton;
@property(nonatomic, strong) NSButton* deleteButton;
@property(nonatomic, strong) NSButton* undoButton;
@property(nonatomic, strong) NSTextField* renameField;
@property(nonatomic, strong) NSMutableArray<ClippPopupRowView*>* registerRowViews;
@property(nonatomic, strong) NSMutableArray<ClippPopupRowView*>* historyRowViews;
@property(nonatomic, strong) ClippPopupSatellitePanel* flyoutPanel;
@property(nonatomic, strong) NSTextField* flyoutText;
@property(nonatomic, strong) NSImageView* flyoutImage;
@property(nonatomic, strong) ClippPopupSatellitePanel* toastPanel;

- (void)toggle;
- (void)dismiss;
- (void)handleF2;
- (BOOL)handleCommandEquivalent:(NSEvent*)event;
- (void)rowClicked:(ClippPopupRowView*)row clickCount:(NSInteger)clicks;
- (NSMenu*)contextMenuForRow:(ClippPopupRowView*)row;
@end

// Defined below the @implementation; declared here for use inside it.
static void PopupActivityWatcher(const ClipboardActivityUpdate& update, void* userData);
static NSAttributedString* HighlightedStringWrapped(NSString* text, NSString* filter);

@implementation ClippPopupMainPanel

- (BOOL)canBecomeKeyWindow {
    return YES;
}

- (void)sendEvent:(NSEvent*)event {
    // F2 straight off the event stream: the filter's field editor otherwise
    // swallows function keys before they reach any responder of ours, which
    // made rename-by-key a coin flip.
    if (event.type == NSEventTypeKeyDown && event.keyCode == kVK_F2) {
        [self.controller handleF2];
        return;
    }
    [super sendEvent:event];
}

- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if ([self.controller handleCommandEquivalent:event]) {
        return YES;
    }
    return [super performKeyEquivalent:event];
}

@end

@implementation ClippPopupRowView

- (void)mouseDown:(NSEvent*)event {
    [self.controller rowClicked:self clickCount:event.clickCount];
}

- (NSMenu*)menuForEvent:(NSEvent*)event {
    (void)event;
    return [self.controller contextMenuForRow:self];
}

@end

// Flipped document view so rows stack from the top (same trick as ClippPage).
@interface ClippPopupFlippedView : NSView
@end

@implementation ClippPopupFlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

@implementation ClippPopupController

- (instancetype)init {
    self = [super init];
    if (self) {
        watcherID_ = 0;
        registersPresent_ = false;
        flyoutUpdatePending_ = false;
        self.registerRowViews = [[NSMutableArray alloc] init];
        self.historyRowViews = [[NSMutableArray alloc] init];
        [self buildPanel];
    }
    return self;
}

// ---- construction ----------------------------------------------------------

- (void)buildPanel {
    ClippPopupMainPanel* panel = [[ClippPopupMainPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, kPopupWidthOneCol, kPopupHeight)
                  styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    panel.controller = self;
    panel.releasedWhenClosed = NO;
    panel.level = NSStatusWindowLevel;
    panel.opaque = NO;
    panel.backgroundColor = [NSColor clearColor];
    panel.hasShadow = YES;
    panel.hidesOnDeactivate = NO;
    panel.becomesKeyOnlyIfNeeded = NO;
    panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces
        | NSWindowCollectionBehaviorFullScreenAuxiliary;
    self.panel = panel;

    NSVisualEffectView* root = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
    root.translatesAutoresizingMaskIntoConstraints = NO;
    root.material = NSVisualEffectMaterialPopover;
    root.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    root.state = NSVisualEffectStateActive;
    root.wantsLayer = YES;
    root.layer.cornerRadius = 12.0;
    root.layer.masksToBounds = YES;
    panel.contentView = root;

    // Identity line: a surprise borderless panel on a stray keystroke should
    // say what it is.
    NSTextField* title = [NSTextField labelWithString:
        CLP_NS(CLP_UI_APP_NAME) @" — " CLP_NS(CLP_UI_TRAY_POPUP)];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightSemibold];
    title.textColor = [NSColor secondaryLabelColor];

    // Toolbar: icon-only, tooltipped; mouse affordances for the keyboard
    // actions (Save is clipboard-stream-only; Rename/privacy register-only).
    self.saveButton = MacOSMakeIconButton(@"square.and.arrow.down", CLP_NS(CLP_UI_POPUP_SAVE_TIP_MAC),
                                          self, @selector(saveClicked:));
    self.activateButton = MacOSMakeIconButton(@"doc.on.clipboard", CLP_NS(CLP_UI_PASTE),
                                              self, @selector(copyClicked:));
    self.renameButton = MacOSMakeIconButton(@"pencil", CLP_NS(CLP_UI_POPUP_RENAME_TIP_MAC),
                                            self, @selector(renameClicked:));
    self.privateButton = MacOSMakeIconButton(@"lock", CLP_NS(CLP_UI_POPUP_MAKE_PRIVATE),
                                             self, @selector(privateClicked:));
    self.deleteButton = MacOSMakeIconButton(@"trash", CLP_NS(CLP_UI_POPUP_DELETE_TIP_MAC),
                                            self, @selector(deleteClicked:));
    self.undoButton = MacOSMakeIconButton(@"arrow.uturn.backward", CLP_NS(CLP_UI_POPUP_UNDO_TIP_MAC),
                                          self, @selector(undoClicked:));
    NSStackView* toolbar = [[NSStackView alloc] initWithFrame:NSZeroRect];
    toolbar.translatesAutoresizingMaskIntoConstraints = NO;
    toolbar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    toolbar.spacing = 2.0;
    [toolbar addArrangedSubview:self.saveButton];
    [toolbar addArrangedSubview:self.activateButton];
    [toolbar addArrangedSubview:self.renameButton];
    [toolbar addArrangedSubview:self.privateButton];
    [toolbar addArrangedSubview:self.deleteButton];
    [toolbar addArrangedSubview:self.undoButton];
    for (NSButton* button in @[ self.saveButton, self.activateButton, self.renameButton,
                                self.privateButton, self.deleteButton, self.undoButton ]) {
        // Toolbar clicks must never move the keyboard out of the filter field
        // (or steal the focus the rename editor is about to take).
        button.refusesFirstResponder = YES;
    }

    // The filter field owns the keyboard for the panel's life (launcher
    // pattern); its delegate routes arrows/Enter/Del/Esc to the model.
    NSSearchField* filter = [[NSSearchField alloc] initWithFrame:NSZeroRect];
    filter.translatesAutoresizingMaskIntoConstraints = NO;
    filter.placeholderString = CLP_NS(CLP_UI_POPUP_FILTER_HINT);
    filter.delegate = self;
    filter.sendsSearchStringImmediately = YES;
    filter.sendsWholeSearchString = NO;
    self.filterField = filter;

    // Two columns: Registers (left, hidden until any exist) | Clipboard.
    self.registersLabel = [self makeColumnLabel:CLP_NS(CLP_UI_POPUP_REGISTERS)];
    self.historyLabel = [self makeColumnLabel:CLP_NS(CLP_UI_CLIPBOARD)];
    self.historyLabel.hidden = YES;

    NSScrollView* registerScroll = [self makeRowsScrollView];
    self.registerScroll = registerScroll;
    self.registerRowsStack = [self rowsStackInScrollView:registerScroll];
    NSScrollView* historyScroll = [self makeRowsScrollView];
    self.historyScroll = historyScroll;
    self.historyRowsStack = [self rowsStackInScrollView:historyScroll];

    NSStackView* registerColumn = [[NSStackView alloc] initWithFrame:NSZeroRect];
    registerColumn.translatesAutoresizingMaskIntoConstraints = NO;
    registerColumn.orientation = NSUserInterfaceLayoutOrientationVertical;
    registerColumn.alignment = NSLayoutAttributeLeading;
    registerColumn.spacing = 4.0;
    [registerColumn addArrangedSubview:self.registersLabel];
    [registerColumn addArrangedSubview:registerScroll];
    [registerScroll.widthAnchor constraintEqualToAnchor:registerColumn.widthAnchor].active = YES;
    self.registerColumn = registerColumn;
    registerColumn.hidden = YES;

    NSStackView* historyColumn = [[NSStackView alloc] initWithFrame:NSZeroRect];
    historyColumn.translatesAutoresizingMaskIntoConstraints = NO;
    historyColumn.orientation = NSUserInterfaceLayoutOrientationVertical;
    historyColumn.alignment = NSLayoutAttributeLeading;
    historyColumn.spacing = 4.0;
    [historyColumn addArrangedSubview:self.historyLabel];
    [historyColumn addArrangedSubview:historyScroll];
    [historyScroll.widthAnchor constraintEqualToAnchor:historyColumn.widthAnchor].active = YES;

    NSStackView* columns = [[NSStackView alloc] initWithFrame:NSZeroRect];
    columns.translatesAutoresizingMaskIntoConstraints = NO;
    columns.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    columns.alignment = NSLayoutAttributeTop;
    columns.distribution = NSStackViewDistributionFillEqually;
    columns.spacing = 8.0;
    // Collapse the hidden registers column out of layout entirely.
    columns.detachesHiddenViews = YES;
    [columns addArrangedSubview:registerColumn];
    [columns addArrangedSubview:historyColumn];
    [registerColumn.heightAnchor constraintEqualToAnchor:columns.heightAnchor].active = YES;
    [historyColumn.heightAnchor constraintEqualToAnchor:columns.heightAnchor].active = YES;

    [root addSubview:title];
    [root addSubview:toolbar];
    [root addSubview:filter];
    [root addSubview:columns];

    [NSLayoutConstraint activateConstraints:@[
        [title.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:14.0],
        [title.trailingAnchor constraintLessThanOrEqualToAnchor:root.trailingAnchor constant:-14.0],
        [title.topAnchor constraintEqualToAnchor:root.topAnchor constant:10.0],

        [toolbar.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:12.0],
        [toolbar.trailingAnchor constraintLessThanOrEqualToAnchor:root.trailingAnchor constant:-12.0],
        [toolbar.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:6.0],

        [filter.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:12.0],
        [filter.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-12.0],
        [filter.topAnchor constraintEqualToAnchor:toolbar.bottomAnchor constant:8.0],

        [columns.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8.0],
        [columns.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8.0],
        [columns.topAnchor constraintEqualToAnchor:filter.bottomAnchor constant:8.0],
        [columns.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-8.0],
    ]];

    [self buildFlyout];
    [self buildToast];
}

- (NSTextField*)makeColumnLabel:(NSString*)text {
    NSTextField* label = [NSTextField labelWithString:text];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightSemibold];
    label.textColor = [NSColor secondaryLabelColor];
    return label;
}

- (NSScrollView*)makeRowsScrollView {
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = NO;
    scroll.autohidesScrollers = YES;
    scroll.borderType = NSNoBorder;
    scroll.drawsBackground = NO;
    return scroll;
}

- (NSStackView*)rowsStackInScrollView:(NSScrollView*)scroll {
    NSView* documentView = [[ClippPopupFlippedView alloc] initWithFrame:NSZeroRect];
    documentView.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.documentView = documentView;

    NSStackView* stack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    // Leading + explicit per-view width pins (appendRow:to:), NOT
    // NSLayoutAttributeWidth: that alignment left views at intrinsic width
    // flushed to the trailing edge (right-aligned lists, shrunken editor).
    stack.alignment = NSLayoutAttributeLeading;
    stack.distribution = NSStackViewDistributionFill;
    stack.spacing = 2.0;
    [documentView addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [documentView.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:documentView.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:documentView.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:documentView.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:documentView.bottomAnchor],
    ]];
    return stack;
}

- (void)buildFlyout {
    ClippPopupSatellitePanel* flyout = [[ClippPopupSatellitePanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 100, 44)
                  styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    flyout.releasedWhenClosed = NO;
    flyout.level = NSStatusWindowLevel;
    flyout.opaque = NO;
    flyout.backgroundColor = [NSColor clearColor];
    flyout.hasShadow = YES;
    flyout.hidesOnDeactivate = NO;
    flyout.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces
        | NSWindowCollectionBehaviorFullScreenAuxiliary;

    NSVisualEffectView* root = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
    root.material = NSVisualEffectMaterialPopover;
    root.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    root.state = NSVisualEffectStateActive;
    root.wantsLayer = YES;
    root.layer.cornerRadius = 10.0;
    root.layer.masksToBounds = YES;
    flyout.contentView = root;

    NSTextField* text = [NSTextField wrappingLabelWithString:@""];
    text.translatesAutoresizingMaskIntoConstraints = NO;
    text.font = [NSFont systemFontOfSize:13.0];
    text.textColor = [NSColor labelColor];
    text.selectable = NO;
    text.preferredMaxLayoutWidth = kFlyoutMaxTextWidth;
    self.flyoutText = text;

    NSImageView* image = [[NSImageView alloc] initWithFrame:NSZeroRect];
    image.translatesAutoresizingMaskIntoConstraints = NO;
    image.imageScaling = NSImageScaleProportionallyDown;
    image.hidden = YES;
    self.flyoutImage = image;

    [root addSubview:text];
    [root addSubview:image];
    [NSLayoutConstraint activateConstraints:@[
        [text.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:10.0],
        [text.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-10.0],
        [text.topAnchor constraintEqualToAnchor:root.topAnchor constant:8.0],
        [text.bottomAnchor constraintLessThanOrEqualToAnchor:root.bottomAnchor constant:-8.0],
        [image.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8.0],
        [image.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8.0],
        [image.topAnchor constraintEqualToAnchor:root.topAnchor constant:8.0],
        [image.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-8.0],
    ]];

    self.flyoutPanel = flyout;
}

- (void)buildToast {
    ClippPopupSatellitePanel* toast = [[ClippPopupSatellitePanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 100, 26)
                  styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    toast.releasedWhenClosed = NO;
    toast.level = NSStatusWindowLevel;
    toast.opaque = NO;
    toast.backgroundColor = [NSColor clearColor];
    toast.hasShadow = YES;
    toast.hidesOnDeactivate = NO;
    toast.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces
        | NSWindowCollectionBehaviorFullScreenAuxiliary;

    NSView* root = [[NSView alloc] initWithFrame:NSZeroRect];
    root.wantsLayer = YES;
    root.layer.backgroundColor = [NSColor colorWithCalibratedWhite:0.16 alpha:0.96].CGColor;
    root.layer.cornerRadius = 12.0;
    toast.contentView = root;

    NSTextField* label = [NSTextField labelWithString:CLP_NS(CLP_UI_POPUP_TOAST_MAC)];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.font = [NSFont systemFontOfSize:12.0];
    label.textColor = [NSColor whiteColor];
    [root addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:14.0],
        [label.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-14.0],
        [label.topAnchor constraintEqualToAnchor:root.topAnchor constant:5.0],
        [label.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-5.0],
    ]];

    self.toastPanel = toast;
}

// ---- lifecycle -------------------------------------------------------------

- (void)toggle {
    if (self.panel.visible) {
        [self dismiss];
    } else {
        [self summon];
    }
}

- (void)summon {
    [self rebuildFromStores];
    [self updateColumnLayoutAndFrame:NO];
    self.filterField.stringValue = @"";
    model_.SetFilter({});
    [self renderList];

    // Center on the screen holding the mouse.
    const NSPoint mouse = [NSEvent mouseLocation];
    NSScreen* screen = [NSScreen mainScreen];
    for (NSScreen* candidate in [NSScreen screens]) {
        if (NSMouseInRect(mouse, candidate.frame, NO)) {
            screen = candidate;
            break;
        }
    }
    const NSRect work = screen.visibleFrame;
    const CGFloat width = registersPresent_ ? kPopupWidthTwoCol : kPopupWidthOneCol;
    const NSRect frame = NSMakeRect(
        work.origin.x + (work.size.width - width) / 2.0,
        work.origin.y + (work.size.height - kPopupHeight) / 2.0,
        width, kPopupHeight);
    [self.panel setFrame:frame display:YES];

    // Key without activating: the previous app keeps focus-ownership; closing
    // the panel hands the keyboard straight back.
    [self.panel makeKeyAndOrderFront:nil];
    [self.panel makeFirstResponder:self.filterField];

    // Light dismiss the moment something else takes the keyboard.
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(panelResignedKey:)
                                                 name:NSWindowDidResignKeyNotification
                                               object:self.panel];

    if (g_settings.popupHintShownCount() < Settings::PopupHintMaxShows) {
        [self showToast];
        g_settings.notePopupHintShown();
    }

    [self scheduleFlyoutUpdate];
    [self beginWatcher];
}

- (void)dismiss {
    if (!self.panel.visible) {
        return;
    }
    if (editingRegister_.has_value()) {
        [self endEditMode];  // silent cancel; the next summon rebuilds
    }
    [[NSNotificationCenter defaultCenter] removeObserver:self
                                                    name:NSWindowDidResignKeyNotification
                                                  object:self.panel];
    [self endWatcher];
    [self.toastPanel orderOut:nil];
    [self.flyoutPanel orderOut:nil];
    // Session-scoped peeks: anything revealed inside the popup is forgotten
    // the moment it hides.
    uiClippPage::ForgetAllPeekedItems();
    [self.panel orderOut:nil];
}

- (void)panelResignedKey:(NSNotification*)notification {
    (void)notification;
    [self dismiss];
}

- (void)destroy {
    [self dismiss];
    [self.flyoutPanel close];
    [self.toastPanel close];
    [self.panel close];
}

// ---- data ------------------------------------------------------------------

- (void)rebuildFromStores {
    registerCache_.clear();
    std::vector<PopupItem> registers;
    auto records = g_registerStore.List();  // live values, name-sorted
    registers.reserve(records.size());
    for (auto& rec : records) {
        if (rec.name.empty()) {
            continue;  // the "" clipboard mirror IS the clipboard column
        }
        RegisterRowInfo info;
        info.name = MacOSToNSString(rec.name);
        info.isPrivate = rec.IsPrivate();
        info.touchedWallMs = rec.touched.wallMs;

        PopupItem item;
        item.kind = PopupItem::Kind::Register;
        item.registerName = rec.name;
        item.actionable = true;
        // Register NAMES are user data — the primary handle — so they
        // participate in find (and light up), unlike history kind-labels.
        item.searchText = Utf8ToWideString(rec.name);

        if (rec.IsPrivate()) {
            info.previewText = @"••••••••";  // fixed width: not length-revealing
        } else if (rec.IsBinary()) {
            RegisterWire::BinaryValueInfo bin{};
            if (RegisterWire::TryParseBinaryValue(rec.value, bin)
                && IsClippImageFormat(bin.formatId)) {
                info.previewText = CLP_NS(CLP_UI_IMAGE);
                info.imageData = [NSData dataWithBytes:rec.value.data() + bin.streamOffset
                                                length:rec.value.size() - bin.streamOffset];
            } else {
                info.previewText = CLP_NS(CLP_UI_UNSUPPORTED_CLIPBOARD_ITEM);
            }
        } else {
            info.contentRow = true;
            info.fullText = MacOSToNSString(rec.value);
            info.previewText = info.fullText.length > popupfind::kRegisterPreviewChars
                ? [[info.fullText substringToIndex:popupfind::kRegisterPreviewChars]
                      stringByAppendingString:@"..."]
                : info.fullText;
            std::wstring searchText = item.searchText;
            searchText += L"\n";
            searchText += Utf8ToWideString(MacOSToStdString(info.previewText));
            item.searchText = std::move(searchText);
        }
        registerCache_.emplace(rec.name, info);
        registers.push_back(std::move(item));
    }
    registersPresent_ = !registers.empty();

    displayCache_.clear();
    std::vector<PopupItem> history;
    const auto snapshot = g_clipboardActivityStore.Snapshot();  // ascending by ts
    history.reserve(snapshot.size());
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
        auto display = g_clipboardActivityStore.DisplayItem(it->id);
        if (!display) {
            continue;
        }
        PopupItem item;
        item.kind = PopupItem::Kind::History;
        item.historyId = it->id;
        // Type-to-find matches CONTENT only — kind labels, device names, and
        // ages are neither located nor highlighted.
        const bool contentKind =
            display->kind == ClipboardActivityPayloadKind::Text ||
            display->kind == ClipboardActivityPayloadKind::Link;
        item.searchText = contentKind ? display->previewText : std::wstring{};
        item.actionable = display->kind != ClipboardActivityPayloadKind::PrivatePlaceholder;
        displayCache_.emplace(it->id, std::move(*display));
        history.push_back(std::move(item));
    }
    model_.SetItems(std::move(registers), std::move(history));
}

// Registers have no store watcher; popup-initiated ops refresh explicitly,
// optionally re-selecting one by name.
- (void)refreshAfterRegisterOpSelecting:(const std::optional<std::string>&)selectName {
    [self rebuildFromStores];
    [self updateColumnLayoutAndFrame:YES];
    if (selectName.has_value()) {
        const auto& regs = model_.VisibleRegisters();
        for (std::size_t i = 0; i < regs.size(); ++i) {
            if (regs[i]->registerName == *selectName) {
                model_.SelectAt(PopupModel::Group::Registers, i);
                break;
            }
        }
    }
    [self renderList];
}

// Reflect registersPresent_ into the column visibility and — when the flip
// happens mid-session — into the panel width, keeping the center.
- (void)updateColumnLayoutAndFrame:(BOOL)resizeIfVisible {
    self.registerColumn.hidden = !registersPresent_;
    self.historyLabel.hidden = !registersPresent_;
    self.registersLabel.hidden = !registersPresent_;

    if (!resizeIfVisible || !self.panel.visible) {
        return;
    }
    const CGFloat width = registersPresent_ ? kPopupWidthTwoCol : kPopupWidthOneCol;
    NSRect frame = self.panel.frame;
    if (frame.size.width == width) {
        return;
    }
    const CGFloat centerX = NSMidX(frame);
    frame.origin.x = centerX - width / 2.0;
    frame.size.width = width;
    [self.panel setFrame:frame display:YES];
}

// ---- rendering -------------------------------------------------------------

- (void)renderList {
    // The teardown below momentarily collapses the documents and the clip
    // views clamp — capture the offsets now, restore after relayout, so a
    // rebuild never MOVES the lists (renaming an in-view row used to bounce
    // the whole column).
    const NSPoint registerOffset = self.registerScroll.contentView.documentVisibleRect.origin;
    const NSPoint historyOffset = self.historyScroll.contentView.documentVisibleRect.origin;

    for (NSView* view in [self.registerRowsStack.arrangedSubviews copy]) {
        [self.registerRowsStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    for (NSView* view in [self.historyRowsStack.arrangedSubviews copy]) {
        [self.historyRowsStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    [self.registerRowViews removeAllObjects];
    [self.historyRowViews removeAllObjects];
    self.renameField = nil;  // re-created below while a rename is in flight

    const auto& registers = model_.VisibleRegisters();
    const std::size_t shownRegisters =
        (std::min)(registers.size(), popupfind::kMaxRenderedRows);
    for (std::size_t i = 0; i < shownRegisters; ++i) {
        [self appendRow:[self buildRegisterRow:*registers[i] index:static_cast<NSInteger>(i)]
                     to:self.registerRowsStack];
    }
    if (registers.size() > shownRegisters) {
        [self appendRow:[self makeMoreHint] to:self.registerRowsStack];
    }

    const auto& history = model_.VisibleHistory();
    const std::size_t shown = (std::min)(history.size(), popupfind::kMaxRenderedRows);
    for (std::size_t i = 0; i < shown; ++i) {
        [self appendRow:[self buildHistoryRow:*history[i] index:static_cast<NSInteger>(i)]
                     to:self.historyRowsStack];
    }
    if (history.size() > shown) {
        [self appendRow:[self makeMoreHint] to:self.historyRowsStack];
    }
    if (history.empty()) {
        NSTextField* empty = [NSTextField wrappingLabelWithString:CLP_NS(CLP_UI_POPUP_EMPTY)];
        empty.translatesAutoresizingMaskIntoConstraints = NO;
        empty.font = [NSFont systemFontOfSize:13.0];
        empty.textColor = [NSColor secondaryLabelColor];
        empty.alignment = NSTextAlignmentCenter;
        [self appendRow:empty to:self.historyRowsStack];
    }

    [self renderHighlight];

    // Row frames exist only after a layout pass; then restore the captured
    // offsets (the clip views clamp them to the fresh extents) and ensure the
    // selection is visible — scrolling ONLY if it actually isn't.
    [self.panel.contentView layoutSubtreeIfNeeded];
    [self.registerScroll.contentView scrollToPoint:registerOffset];
    [self.registerScroll reflectScrolledClipView:self.registerScroll.contentView];
    [self.historyScroll.contentView scrollToPoint:historyOffset];
    [self.historyScroll reflectScrolledClipView:self.historyScroll.contentView];
    [self scrollListsToSelection];

    // Focus the freshly built inline rename editor, preselecting the name so
    // typing replaces it. Deferred a runloop turn: grabbing first responder
    // synchronously inside a button action / fresh render loses the race
    // often enough to feel broken.
    if (self.renameField != nil) {
        NSTextField* editor = self.renameField;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (editor == self.renameField && editor.window == self.panel) {
                [editor selectText:nil];  // takes first responder AND selects all
            }
        });
    }
}

- (void)scrollToTop:(NSScrollView*)scroll {
    if (scroll.documentView == nil) {
        return;
    }
    [scroll.contentView scrollToPoint:NSMakePoint(0, 0)];  // flipped doc: (0,0) = top
    [scroll reflectScrolledClipView:scroll.contentView];
}

- (void)scrollListsToSelection {
    const auto selection = model_.Selected();
    ClippPopupRowView* row = [self selectedRowView];
    if (selection.has_value() && row != nil) {
        // Only move the column when the row is NOT already fully in view — a
        // no-op keeps the list rock-steady across rebuilds (rename, refresh).
        NSScrollView* own = selection->group == PopupModel::Group::Registers
            ? self.registerScroll : self.historyScroll;
        const NSRect rowInDocument = [row convertRect:row.bounds toView:own.documentView];
        if (!NSContainsRect(own.contentView.documentVisibleRect, rowInDocument)) {
            [row scrollRectToVisible:row.bounds];
        }
    } else {
        [self scrollToTop:self.registerScroll];
        [self scrollToTop:self.historyScroll];
    }
}

- (NSTextField*)makeMoreHint {
    NSTextField* more = [NSTextField labelWithString:CLP_NS(CLP_UI_POPUP_MORE)];
    more.translatesAutoresizingMaskIntoConstraints = NO;
    more.font = [NSFont systemFontOfSize:12.0];
    more.textColor = [NSColor secondaryLabelColor];
    return more;
}

// addArrangedSubview + explicit full-width pin (see rowsStackInScrollView on
// why alignment can't be trusted for this).
- (void)appendRow:(NSView*)view to:(NSStackView*)stack {
    [stack addArrangedSubview:view];
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

- (ClippPopupRowView*)makeRowWithGroup:(NSInteger)group index:(NSInteger)index {
    ClippPopupRowView* row = [[ClippPopupRowView alloc] initWithFrame:NSZeroRect];
    row.translatesAutoresizingMaskIntoConstraints = NO;
    row.controller = self;
    row.group = group;
    row.index = index;
    row.wantsLayer = YES;
    row.layer.cornerRadius = 6.0;
    return row;
}

- (NSTextField*)makeRowLine:(NSAttributedString*)text {
    NSTextField* label = [NSTextField labelWithAttributedString:text];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.maximumNumberOfLines = 1;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.allowsDefaultTighteningForTruncation = NO;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

- (void)packRow:(ClippPopupRowView*)row lines:(NSArray<NSView*>*)lines {
    NSStackView* content = [[NSStackView alloc] initWithFrame:NSZeroRect];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    content.orientation = NSUserInterfaceLayoutOrientationVertical;
    content.alignment = NSLayoutAttributeLeading;
    content.spacing = 1.0;
    for (NSView* line in lines) {
        [content addArrangedSubview:line];
        // Explicit full-width pin: labels truncate in place, text draws from
        // the left, and the inline rename editor gets the whole row to type
        // in. (Stack alignment Width proved unreliable for this.)
        [line.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    }
    [row addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:row.leadingAnchor constant:10.0],
        [content.trailingAnchor constraintEqualToAnchor:row.trailingAnchor constant:-10.0],
        [content.topAnchor constraintEqualToAnchor:row.topAnchor constant:5.0],
        [content.bottomAnchor constraintEqualToAnchor:row.bottomAnchor constant:-5.0],
    ]];
}

- (NSView*)buildHistoryRow:(const PopupItem&)item index:(NSInteger)index {
    ClippPopupRowView* row = [self makeRowWithGroup:1 index:index];

    NSString* filter = self.filterField.stringValue;
    NSString* previewText = @" ";
    NSString* metaText = @"";
    bool contentRow = false;
    const auto cached = displayCache_.find(item.historyId);
    if (cached != displayCache_.end()) {
        const auto& display = cached->second;
        switch (display.kind) {
        case ClipboardActivityPayloadKind::Image:
            previewText = CLP_NS(CLP_UI_IMAGE);
            break;
        case ClipboardActivityPayloadKind::PrivatePlaceholder:
            previewText = CLP_NS(CLP_UI_PRIVATE_PLACEHOLDER_TITLE);
            break;
        default:
            previewText = MacOSToNSString(display.previewText);
            break;
        }
        contentRow = display.kind == ClipboardActivityPayloadKind::Text ||
                     display.kind == ClipboardActivityPayloadKind::Link;
        metaText = display.deviceName.empty()
            ? RelativeAgeFromTimePointNS(display.header.timestamp)
            : [NSString stringWithFormat:@"%@ %@",
                  MacOSToNSString(display.deviceName),
                  RelativeAgeFromTimePointNS(display.header.timestamp)];
    }
    if (previewText.length == 0) {
        previewText = @" ";
    }
    if (contentRow) {
        previewText = ReWindowRowTextNS(previewText, filter);
    }

    NSTextField* preview = [self makeRowLine:
        HighlightedString(previewText, contentRow ? filter : @"",
                          [NSFont systemFontOfSize:13.0], [NSColor labelColor])];

    NSTextField* meta = [self makeRowLine:
        HighlightedString(metaText, @"",
                          [NSFont systemFontOfSize:11.0], [NSColor secondaryLabelColor])];

    [self packRow:row lines:@[preview, meta]];
    [self.historyRowViews addObject:row];
    return row;
}

- (NSView*)buildRegisterRow:(const PopupItem&)item index:(NSInteger)index {
    ClippPopupRowView* row = [self makeRowWithGroup:0 index:index];

    NSString* filter = self.filterField.stringValue;
    const auto cached = registerCache_.find(item.registerName);
    NSString* nameText = cached != registerCache_.end() ? cached->second.name : @" ";

    NSMutableArray<NSView*>* lines = [[NSMutableArray alloc] init];

    const bool editing =
        editingRegister_.has_value() && *editingRegister_ == item.registerName;
    if (editing) {
        NSTextField* editor = [[NSTextField alloc] initWithFrame:NSZeroRect];
        editor.translatesAutoresizingMaskIntoConstraints = NO;
        editor.font = [NSFont systemFontOfSize:13.0];
        editor.stringValue = nameText;
        editor.delegate = self;
        // Single line, horizontally scrolling — a raw NSTextField's cell
        // otherwise WRAPS long names to a second (invisible) line.
        editor.usesSingleLineMode = YES;
        editor.cell.wraps = NO;
        editor.cell.scrollable = YES;
        editor.wantsLayer = YES;
        editor.layer.cornerRadius = 3.0;
        self.renameField = editor;
        [lines addObject:editor];
    } else {
        NSTextField* name = [self makeRowLine:
            HighlightedString(nameText, filter,
                              [NSFont systemFontOfSize:13.0 weight:NSFontWeightSemibold],
                              [NSColor labelColor])];
        [lines addObject:name];
    }

    NSString* previewText =
        cached != registerCache_.end() ? cached->second.previewText : @" ";
    if (previewText.length == 0) {
        previewText = @" ";
    }
    const bool contentRow = cached != registerCache_.end() && cached->second.contentRow;
    if (contentRow) {
        previewText = ReWindowRowTextNS(previewText, filter);
    }
    NSTextField* preview = [self makeRowLine:
        HighlightedString(previewText, contentRow ? filter : @"",
                          [NSFont systemFontOfSize:13.0],
                          [NSColor labelColor])];
    preview.alphaValue = 0.75;  // the name is the row's headline
    [lines addObject:preview];

    if (cached != registerCache_.end()) {
        NSString* metaText = RelativeAgeNS(cached->second.touchedWallMs);
        if (cached->second.isPrivate) {
            metaText = [metaText stringByAppendingFormat:@" · %@", CLP_NS(CLP_UI_PRIVATE_BADGE)];
        }
        NSTextField* meta = [self makeRowLine:
            HighlightedString(metaText, @"",
                              [NSFont systemFontOfSize:11.0], [NSColor secondaryLabelColor])];
        [lines addObject:meta];
    }

    [self packRow:row lines:lines];
    [self.registerRowViews addObject:row];
    return row;
}

- (void)renderHighlight {
    const auto selection = model_.Selected();
    NSColor* selectedColor = [[NSColor grayColor] colorWithAlphaComponent:0.22];
    const auto paint = [&](NSArray<ClippPopupRowView*>* rows, PopupModel::Group group) {
        for (ClippPopupRowView* row in rows) {
            const bool selected = selection.has_value()
                && selection->group == group
                && selection->index == static_cast<std::size_t>(row.index);
            row.layer.backgroundColor = selected
                ? selectedColor.CGColor : [NSColor clearColor].CGColor;
            if (selected) {
                [row scrollRectToVisible:row.bounds];
            }
        }
    };
    paint(self.registerRowViews, PopupModel::Group::Registers);
    paint(self.historyRowViews, PopupModel::Group::History);
    [self updateToolbar];
    [self scheduleFlyoutUpdate];
}

- (void)updateToolbar {
    const PopupItem* item = model_.SelectedItem();
    bool canSave = false;
    if (item != nullptr && item->kind == PopupItem::Kind::History && item->actionable) {
        const auto cached = displayCache_.find(item->historyId);
        if (cached != displayCache_.end()) {
            const auto kind = cached->second.kind;
            canSave = kind == ClipboardActivityPayloadKind::Text
                || kind == ClipboardActivityPayloadKind::Link
                || kind == ClipboardActivityPayloadKind::Image
                || kind == ClipboardActivityPayloadKind::PrivateText;
        }
    }
    const bool registerSelected =
        item != nullptr && item->kind == PopupItem::Kind::Register;
    bool selectedPrivate = false;
    if (registerSelected) {
        const auto cached = registerCache_.find(item->registerName);
        selectedPrivate = cached != registerCache_.end() && cached->second.isPrivate;
    }
    self.saveButton.enabled = canSave;
    self.activateButton.enabled = item != nullptr && item->actionable;
    self.renameButton.enabled = registerSelected;
    self.privateButton.enabled = registerSelected;
    // The button shows the ACTION: lock a public register, unlock a private one.
    self.privateButton.image = MacOSMakeSymbolImage(
        selectedPrivate ? @"lock.open" : @"lock",
        selectedPrivate ? CLP_NS(CLP_UI_POPUP_MAKE_PUBLIC) : CLP_NS(CLP_UI_POPUP_MAKE_PRIVATE),
        13.0, [NSColor secondaryLabelColor]);
    self.privateButton.toolTip = selectedPrivate
        ? CLP_NS(CLP_UI_POPUP_MAKE_PUBLIC) : CLP_NS(CLP_UI_POPUP_MAKE_PRIVATE);
    self.deleteButton.enabled = item != nullptr;

    const auto undoKind = clipp::PendingUndoKind();
    self.undoButton.enabled = undoKind != clipp::UndoSlotKind::None;
    NSString* undoTip = CLP_NS(CLP_UI_POPUP_UNDO_TIP_MAC);
    if (undoKind == clipp::UndoSlotKind::Register) {
        const std::string label = clipp::PendingUndoLabel();
        if (!label.empty()) {
            undoTip = [NSString stringWithFormat:@"%@\"%@\" (⌘Z)",
                CLP_NS(CLP_UI_POPUP_UNDO_OF_PREFIX), MacOSToNSString(label)];
        }
    }
    self.undoButton.toolTip = undoTip;
}

// ---- flyout + toast --------------------------------------------------------

// The flyout anchors to the selected row's on-screen frame, which for freshly
// built rows only exists after layout — force it, then defer a hop (coalesced).
- (void)scheduleFlyoutUpdate {
    if (flyoutUpdatePending_) {
        return;
    }
    flyoutUpdatePending_ = true;
    dispatch_async(dispatch_get_main_queue(), ^{
        self->flyoutUpdatePending_ = false;
        [self updateFlyout];
    });
}

- (ClippPopupRowView*)selectedRowView {
    const auto selection = model_.Selected();
    if (!selection.has_value()) {
        return nil;
    }
    NSArray<ClippPopupRowView*>* rows = selection->group == PopupModel::Group::Registers
        ? self.registerRowViews : self.historyRowViews;
    if (selection->index >= rows.count) {
        return nil;
    }
    return rows[selection->index];
}

- (void)updateFlyout {
    const PopupItem* item = model_.SelectedItem();
    if (item == nullptr || !self.panel.visible || editingRegister_.has_value()) {
        [self.flyoutPanel orderOut:nil];
        return;
    }

    if (item->kind == PopupItem::Kind::Register) {
        const auto cached = registerCache_.find(item->registerName);
        if (cached == registerCache_.end()) {
            [self.flyoutPanel orderOut:nil];
            return;
        }
        const auto& info = cached->second;
        if (info.imageData != nil) {
            NSImage* image = [[NSImage alloc] initWithData:info.imageData];
            if (image != nil) {
                [self showFlyoutImage:image preferLeft:YES];
                return;
            }
            [self.flyoutPanel orderOut:nil];
            return;
        }
        if (!info.contentRow || TextFitsInRowNS(info.fullText)) {
            [self.flyoutPanel orderOut:nil];
            return;
        }
        [self showFlyoutText:WindowAroundFirstMatchNS(info.fullText, self.filterField.stringValue)
                  preferLeft:YES];
        return;
    }

    const auto cached = displayCache_.find(item->historyId);
    if (cached == displayCache_.end()) {
        [self.flyoutPanel orderOut:nil];
        return;
    }
    const auto& display = cached->second;
    if (display.kind == ClipboardActivityPayloadKind::Image && display.imageData
        && !display.imageData->empty()) {
        NSData* data = [NSData dataWithBytes:display.imageData->data()
                                      length:display.imageData->size()];
        NSImage* image = [[NSImage alloc] initWithData:data];
        if (image != nil) {
            [self showFlyoutImage:image preferLeft:NO];
            return;
        }
        [self.flyoutPanel orderOut:nil];
        return;
    }
    if (display.kind != ClipboardActivityPayloadKind::Text &&
        display.kind != ClipboardActivityPayloadKind::Link) {
        [self.flyoutPanel orderOut:nil];
        return;
    }
    NSString* full = MacOSToNSString(
        display.detailText.empty() ? display.previewText : display.detailText);
    if (TextFitsInRowNS(full)) {
        [self.flyoutPanel orderOut:nil];
        return;
    }
    [self showFlyoutText:WindowAroundFirstMatchNS(full, self.filterField.stringValue)
              preferLeft:NO];
}

- (void)showFlyoutText:(NSString*)text preferLeft:(BOOL)preferLeft {
    self.flyoutImage.hidden = YES;
    self.flyoutImage.image = nil;
    self.flyoutText.hidden = NO;
    self.flyoutText.attributedStringValue = HighlightedStringWrapped(
        text, self.filterField.stringValue);

    // Content-size the panel: the wrapping cell's size at the max width, capped.
    const NSSize fit = [self.flyoutText.cell
        cellSizeForBounds:NSMakeRect(0, 0, kFlyoutMaxTextWidth, kFlyoutMaxHeight)];
    const CGFloat width = (std::min)(kFlyoutMaxTextWidth, (std::max)((CGFloat)120.0, fit.width)) + 20.0;
    const CGFloat height = (std::min)(kFlyoutMaxHeight, fit.height) + 16.0;
    [self placeFlyoutWithSize:NSMakeSize(width, height) preferLeft:preferLeft];
}

- (void)showFlyoutImage:(NSImage*)image preferLeft:(BOOL)preferLeft {
    self.flyoutText.hidden = YES;
    self.flyoutText.stringValue = @"";
    self.flyoutImage.hidden = NO;
    self.flyoutImage.image = image;

    NSSize size = image.size;
    if (size.width <= 0 || size.height <= 0) {
        size = NSMakeSize(120, 120);
    }
    const CGFloat scale = (std::min)((CGFloat)1.0,
        (std::min)(kFlyoutMaxTextWidth / size.width, kFlyoutMaxHeight / size.height));
    [self placeFlyoutWithSize:NSMakeSize(size.width * scale + 16.0,
                                         size.height * scale + 16.0)
                   preferLeft:preferLeft];
}

- (void)placeFlyoutWithSize:(NSSize)size preferLeft:(BOOL)preferLeft {
    [self.panel layoutIfNeeded];
    const NSRect panelFrame = self.panel.frame;
    NSScreen* screen = self.panel.screen != nil ? self.panel.screen : [NSScreen mainScreen];
    const NSRect work = screen.visibleFrame;

    // Anchor the flyout's TOP to the selected row's top; register rows open
    // towards their own column's side (the panel's left).
    CGFloat anchorTop = NSMaxY(panelFrame) - 80.0;
    ClippPopupRowView* row = [self selectedRowView];
    if (row != nil && row.window == self.panel) {
        const NSRect rowOnScreen =
            [self.panel convertRectToScreen:[row convertRect:row.bounds toView:nil]];
        anchorTop = NSMaxY(rowOnScreen);
    }
    // Belt: a row scrolled out of view converts to coordinates beyond the
    // panel — never anchor outside the panel's own vertical span.
    anchorTop = (std::min)(anchorTop, NSMaxY(panelFrame) - 20.0);
    anchorTop = (std::max)(anchorTop, panelFrame.origin.y + 60.0);

    CGFloat x;
    if (preferLeft) {
        x = panelFrame.origin.x - size.width - kFlyoutGap;
        if (x < work.origin.x) {
            x = NSMaxX(panelFrame) + kFlyoutGap;
        }
    } else {
        x = NSMaxX(panelFrame) + kFlyoutGap;
        if (x + size.width > NSMaxX(work)) {
            x = panelFrame.origin.x - size.width - kFlyoutGap;
        }
    }
    CGFloat y = anchorTop - size.height;
    if (y < work.origin.y) {
        y = work.origin.y;
    }
    if (y + size.height > NSMaxY(work)) {
        y = NSMaxY(work) - size.height;
    }

    [self.flyoutPanel setFrame:NSMakeRect(x, y, size.width, size.height) display:YES];
    [self.flyoutPanel orderFront:nil];
}

- (void)showToast {
    NSView* root = self.toastPanel.contentView;
    [root layoutSubtreeIfNeeded];
    const NSSize fit = [root fittingSize];
    const NSRect panelFrame = self.panel.frame;
    const NSRect frame = NSMakeRect(
        panelFrame.origin.x + (panelFrame.size.width - fit.width) / 2.0,
        NSMaxY(panelFrame) + 10.0,
        fit.width, fit.height);
    [self.toastPanel setFrame:frame display:YES];
    [self.toastPanel orderFront:nil];
}

- (void)dismissToast {
    [self.toastPanel orderOut:nil];
}

// ---- actions ---------------------------------------------------------------

- (void)activateSelected {
    [self dismissToast];
    [self commitOrCancelRename];
    const PopupItem* item = model_.SelectedItem();
    if (item == nullptr || !item->actionable) {
        return;
    }
    if (item->kind == PopupItem::Kind::History) {
        clipp::ReshareActivityItem(item->historyId);
    } else {
        clipp::MakeRegisterCurrent(item->registerName);
    }
    [self dismiss];
}

- (void)deleteSelected {
    [self dismissToast];
    [self commitOrCancelRename];
    const PopupItem* item = model_.SelectedItem();
    if (item == nullptr) {
        return;
    }
    if (item->kind == PopupItem::Kind::History) {
        clipp::DeleteActivityItemEverywhere(item->historyId);
        // The watcher event rebuilds the list.
    } else {
        clipp::DeleteRegisterEverywhere(item->registerName);
        [self refreshAfterRegisterOpSelecting:std::nullopt];
    }
}

// "Save": promote the selected clipboard item into an auto-named register and
// drop straight into naming it.
- (void)saveSelected {
    [self dismissToast];
    [self commitOrCancelRename];
    const PopupItem* item = model_.SelectedItem();
    if (item == nullptr || item->kind != PopupItem::Kind::History || !item->actionable) {
        return;
    }
    const auto cached = displayCache_.find(item->historyId);
    if (cached == displayCache_.end()) {
        return;
    }
    const auto kind = cached->second.kind;
    const bool saveable = kind == ClipboardActivityPayloadKind::Text
        || kind == ClipboardActivityPayloadKind::Link
        || kind == ClipboardActivityPayloadKind::Image
        || kind == ClipboardActivityPayloadKind::PrivateText;
    if (!saveable) {
        return;
    }
    const bool markPrivate = cached->second.sourceMarked
        || kind == ClipboardActivityPayloadKind::PrivateText;

    const std::string name = NextAutoRegisterName(g_registerStore.ListNames());
    if (!clipp::SaveActivityItemAsRegister(item->historyId, name, markPrivate)) {
        return;
    }

    // Pivot to the result: the new row must be visible, selected, renameable.
    if (self.filterField.stringValue.length > 0) {
        self.filterField.stringValue = @"";
        model_.SetFilter({});
    }
    editingRegister_ = name;
    model_.EnterEditMode();
    [self refreshAfterRegisterOpSelecting:name];
}

- (void)toggleSelectedRegisterPrivate {
    [self dismissToast];
    [self commitOrCancelRename];
    const PopupItem* item = model_.SelectedItem();
    if (item == nullptr || item->kind != PopupItem::Kind::Register) {
        return;
    }
    const std::string name = item->registerName;  // survives the refresh below
    const auto cached = registerCache_.find(name);
    const bool isPrivate = cached != registerCache_.end() && cached->second.isPrivate;
    if (clipp::SetRegisterPrivate(name, !isPrivate)) {
        [self refreshAfterRegisterOpSelecting:name];
        [self.panel makeFirstResponder:self.filterField];
    }
}

- (void)undoLastDelete {
    [self dismissToast];
    [self commitOrCancelRename];
    const auto undoKind = clipp::PendingUndoKind();
    const std::string restoredRegister = clipp::PendingUndoLabel();
    uint64_t restoredItemID = 0;
    if (!clipp::TryUndoDelete(&restoredItemID)) {
        return;
    }
    // Select the resurrected item and bring it into view — the whole point is
    // showing the user their data is back.
    [self rebuildFromStores];
    [self updateColumnLayoutAndFrame:YES];
    if (undoKind == clipp::UndoSlotKind::Register) {
        const auto& regs = model_.VisibleRegisters();
        for (std::size_t i = 0; i < regs.size(); ++i) {
            if (regs[i]->registerName == restoredRegister) {
                model_.SelectAt(PopupModel::Group::Registers, i);
                break;
            }
        }
    } else if (undoKind == clipp::UndoSlotKind::Activity && restoredItemID != 0) {
        const auto& history = model_.VisibleHistory();
        for (std::size_t i = 0; i < history.size(); ++i) {
            if (history[i]->historyId == restoredItemID) {
                model_.SelectAt(PopupModel::Group::History, i);
                break;
            }
        }
    }
    [self renderList];
    [self.panel makeFirstResponder:self.filterField];
}

- (void)saveClicked:(id)sender { (void)sender; [self saveSelected]; }
- (void)copyClicked:(id)sender { (void)sender; [self activateSelected]; }
- (void)renameClicked:(id)sender { (void)sender; [self beginRenameSelected]; }
- (void)privateClicked:(id)sender { (void)sender; [self toggleSelectedRegisterPrivate]; }
- (void)deleteClicked:(id)sender {
    (void)sender;
    [self deleteSelected];
    [self.panel makeFirstResponder:self.filterField];
}
- (void)undoClicked:(id)sender { (void)sender; [self undoLastDelete]; }

// ---- inline rename ---------------------------------------------------------

- (void)beginRenameSelected {
    [self dismissToast];
    if (editingRegister_.has_value()) {
        return;
    }
    const PopupItem* item = model_.SelectedItem();
    if (item == nullptr || item->kind != PopupItem::Kind::Register) {
        return;
    }
    editingRegister_ = item->registerName;
    model_.EnterEditMode();
    [self renderList];  // the selected row re-renders as the editor
}

// The normalized/trimmed editor text, if it names a legal rename target.
- (bool)nameEditorTarget:(std::string&)outName {
    if (self.renameField == nil || !editingRegister_.has_value()) {
        return false;
    }
    std::string name = MacOSToStdString(self.renameField.stringValue);
    const std::size_t first = name.find_first_not_of(' ');
    if (first == std::string::npos) {
        outName.clear();
        return false;
    }
    const std::size_t last = name.find_last_not_of(' ');
    name = name.substr(first, last - first + 1);
    name = clipp_platform_detail::NormalizeUtf8Canonical(name);
    outName = name;
    if (!IsValidRegisterName(name)) {
        return false;
    }
    if (name != *editingRegister_ && registerCache_.count(name) > 0) {
        return false;  // would silently overwrite a sibling
    }
    return true;
}

- (void)validateNameEditor {
    if (self.renameField == nil) {
        return;
    }
    std::string ignored;
    const bool ok = [self nameEditorTarget:ignored];
    self.renameField.layer.borderWidth = ok ? 0.0 : 1.5;
    self.renameField.layer.borderColor =
        [NSColor colorWithCalibratedRed:0.78 green:0.24 blue:0.24 alpha:1.0].CGColor;
}

- (void)commitRenameKeepingSelection:(BOOL)keepSelection {
    if (!editingRegister_.has_value()) {
        return;
    }
    std::string newName;
    if (![self nameEditorTarget:newName]) {
        [self validateNameEditor];  // stay in the editor, painted invalid
        return;
    }
    const std::string oldName = *editingRegister_;
    [self endEditMode];
    if (newName != oldName) {
        clipp::RenameRegister(oldName, newName);
    }
    [self refreshAfterRegisterOpSelecting:
        (keepSelection ? std::optional<std::string>(newName) : std::nullopt)];
    [self.panel makeFirstResponder:self.filterField];
}

- (void)cancelRename {
    if (!editingRegister_.has_value()) {
        return;
    }
    [self endEditMode];
    [self refreshAfterRegisterOpSelecting:std::nullopt];
    [self.panel makeFirstResponder:self.filterField];
}

// Click-away and action-supersede: commit if valid, abandon if not — never
// trap the user in a red box.
- (void)commitOrCancelRename {
    if (!editingRegister_.has_value()) {
        return;
    }
    std::string newName;
    if ([self nameEditorTarget:newName]) {
        [self commitRenameKeepingSelection:NO];
    } else {
        [self cancelRename];
    }
}

- (void)endEditMode {
    editingRegister_.reset();
    self.renameField = nil;
    model_.LeaveEditMode();
}

// ---- keyboard --------------------------------------------------------------

- (void)handleF2 {
    [self beginRenameSelected];
}

- (BOOL)handleCommandEquivalent:(NSEvent*)event {
    if ((event.modifierFlags & NSEventModifierFlagCommand) == 0) {
        return NO;
    }
    NSString* chars = event.charactersIgnoringModifiers.lowercaseString;
    if ([chars isEqualToString:@"s"]) {
        [self saveSelected];
        return YES;
    }
    if ([chars isEqualToString:@"z"]
        && clipp::PendingUndoKind() != clipp::UndoSlotKind::None) {
        [self undoLastDelete];
        return YES;  // unarmed ⌘Z stays the field editor's text-undo
    }
    return NO;
}

// Arrows / Enter / Del / Esc, routed from BOTH the filter field and the
// rename editor's field editors.
- (BOOL)control:(NSControl*)control
       textView:(NSTextView*)textView
    doCommandBySelector:(SEL)commandSelector {
    (void)textView;

    // The keyboard lives in these two fields for the panel's whole life
    // (launcher pattern): swallowing Tab kills both the focus wander and the
    // focus-ring flicker it caused.
    if (commandSelector == @selector(insertTab:)
        || commandSelector == @selector(insertBacktab:)) {
        return YES;
    }

    if (control == self.renameField) {
        if (commandSelector == @selector(insertNewline:)) {
            [self commitRenameKeepingSelection:YES];
            return YES;
        }
        if (commandSelector == @selector(cancelOperation:)) {
            [self cancelRename];
            return YES;
        }
        return NO;
    }

    if (control != self.filterField) {
        return NO;
    }
    [self dismissToast];
    const BOOL filterEmpty = self.filterField.stringValue.length == 0;

    if (commandSelector == @selector(moveDown:)) {
        model_.MoveDown();
        [self renderHighlight];
        return YES;
    }
    if (commandSelector == @selector(moveUp:)) {
        model_.MoveUp();
        [self renderHighlight];
        return YES;
    }
    if (commandSelector == @selector(moveLeft:) && filterEmpty) {
        model_.MoveLeft();
        [self renderHighlight];
        return YES;
    }
    if (commandSelector == @selector(moveRight:) && filterEmpty) {
        model_.MoveRight();
        [self renderHighlight];
        return YES;
    }
    if (commandSelector == @selector(insertNewline:)) {
        [self activateSelected];
        return YES;
    }
    if (commandSelector == @selector(deleteForward:) && filterEmpty) {
        // Forward-delete on an empty filter deletes the selected item
        // everywhere; with filter text it edits the text.
        [self deleteSelected];
        return YES;
    }
    if (commandSelector == @selector(cancelOperation:)) {
        const auto result = model_.HandleEscape();
        if (result == PopupModel::EscapeResult::ClearedFilter) {
            self.filterField.stringValue = @"";
            [self renderList];
        } else if (result == PopupModel::EscapeResult::Close) {
            [self dismiss];
        }
        return YES;
    }
    return NO;
}

- (void)controlTextDidChange:(NSNotification*)notification {
    NSTextField* field = notification.object;
    if (field == self.renameField) {
        [self validateNameEditor];
        return;
    }
    if (field == self.filterField) {
        [self dismissToast];
        model_.SetFilter(Utf8ToWideString(MacOSToStdString(self.filterField.stringValue)));
        [self renderList];
    }
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    // Focus left the rename editor by click-away or tab: commit-if-valid.
    // (Enter and Esc were already handled in doCommandBySelector and cleared
    // editingRegister_, making this a no-op for those paths.)
    if (notification.object == self.renameField) {
        [self commitOrCancelRename];
    }
}

// ---- rows ------------------------------------------------------------------

- (void)rowClicked:(ClippPopupRowView*)row clickCount:(NSInteger)clicks {
    [self dismissToast];
    if (editingRegister_.has_value()) {
        // A click that lands anywhere outside the name editor ends the rename
        // (the editor's own field editor doesn't route here).
        [self commitOrCancelRename];
    }
    model_.SelectAt(row.group == 0 ? PopupModel::Group::Registers : PopupModel::Group::History,
                    static_cast<std::size_t>(row.index));
    if (clicks >= 2) {
        [self activateSelected];
        return;
    }
    [self renderHighlight];
}

- (NSMenu*)contextMenuForRow:(ClippPopupRowView*)row {
    model_.SelectAt(row.group == 0 ? PopupModel::Group::Registers : PopupModel::Group::History,
                    static_cast<std::size_t>(row.index));
    [self renderHighlight];

    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem* pasteItem = [[NSMenuItem alloc] initWithTitle:CLP_NS(CLP_UI_PASTE)
                                                       action:@selector(copyClicked:)
                                                keyEquivalent:@""];
    pasteItem.target = self;
    [menu addItem:pasteItem];

    if (row.group == 0) {
        NSMenuItem* renameItem = [[NSMenuItem alloc] initWithTitle:CLP_NS(CLP_UI_POPUP_RENAME)
                                                            action:@selector(renameClicked:)
                                                     keyEquivalent:@""];
        renameItem.target = self;
        [menu addItem:renameItem];

        const PopupItem* item = model_.SelectedItem();
        bool isPrivate = false;
        if (item != nullptr) {
            const auto cached = registerCache_.find(item->registerName);
            isPrivate = cached != registerCache_.end() && cached->second.isPrivate;
        }
        NSMenuItem* privateItem = [[NSMenuItem alloc]
            initWithTitle:(isPrivate ? CLP_NS(CLP_UI_POPUP_MAKE_PUBLIC)
                                     : CLP_NS(CLP_UI_POPUP_MAKE_PRIVATE))
                   action:@selector(privateClicked:)
            keyEquivalent:@""];
        privateItem.target = self;
        [menu addItem:privateItem];
    }

    NSMenuItem* deleteItem = [[NSMenuItem alloc] initWithTitle:CLP_NS(CLP_UI_DELETE)
                                                        action:@selector(deleteClicked:)
                                                 keyEquivalent:@""];
    deleteItem.target = self;
    [menu addItem:deleteItem];
    return menu;
}

// ---- store watcher (visible only) ------------------------------------------

- (void)beginWatcher {
    if (watcherID_ != 0) {
        return;
    }
    const auto registration =
        g_clipboardActivityStore.QueryAndRegister(&PopupActivityWatcher, (__bridge void*)self);
    watcherID_ = registration.watcherID;
}

- (void)endWatcher {
    if (watcherID_ == 0) {
        return;
    }
    g_clipboardActivityStore.Unregister(watcherID_);
    watcherID_ = 0;
}

static void PopupActivityWatcher(const ClipboardActivityUpdate& update, void* userData) {
    (void)update;
    ClippPopupController* controller = (__bridge ClippPopupController*)userData;
    if (controller == nil) {
        return;
    }
    // Coarse but correct: any store change re-snapshots while visible —
    // except mid-rename, where a re-render would eat the user's typing.
    dispatch_async(dispatch_get_main_queue(), ^{
        if (controller.panel.visible && !controller->editingRegister_.has_value()) {
            [controller rebuildFromStores];
            [controller updateColumnLayoutAndFrame:YES];
            [controller renderList];
        }
    });
}

// Flyout text with highlight, wrapping variant (the row helper truncates).
static NSAttributedString* HighlightedStringWrapped(NSString* text, NSString* filter) {
    NSMutableParagraphStyle* paragraph = [[NSMutableParagraphStyle alloc] init];
    paragraph.lineBreakMode = NSLineBreakByWordWrapping;
    NSMutableAttributedString* result = [[NSMutableAttributedString alloc]
        initWithString:(text != nil ? text : @"")
            attributes:@{
                NSFontAttributeName: [NSFont systemFontOfSize:13.0],
                NSForegroundColorAttributeName: [NSColor labelColor],
                NSParagraphStyleAttributeName: paragraph,
            }];
    if (filter.length > 0) {
        for (const NSRange& range : FindMatchesNS(text, filter)) {
            [result addAttributes:@{
                NSBackgroundColorAttributeName:
                    [NSColor colorWithCalibratedRed:1.0 green:0.725 blue:0.0 alpha:0.59],
                NSForegroundColorAttributeName: [NSColor blackColor],
            } range:range];
        }
    }
    return result;
}

@end

// ---- entry points ----------------------------------------------------------

namespace {

ClippPopupController* g_popupController = nil;
EventHotKeyRef g_hotKeyPrimary = nullptr;
EventHotKeyRef g_hotKeySecondary = nullptr;
EventHandlerRef g_hotKeyHandler = nullptr;

OSStatus PopupHotKeyHandler(EventHandlerCallRef nextHandler, EventRef event, void* userData) {
    (void)nextHandler;
    (void)event;
    (void)userData;
    dispatch_async(dispatch_get_main_queue(), ^{
        clipp::TogglePopupPanel();
    });
    return noErr;
}

}  // namespace

namespace clipp {

void InstallPopupHotkeys() {
    if (g_hotKeyHandler != nullptr) {
        return;
    }
    EventTypeSpec spec{ kEventClassKeyboard, kEventHotKeyPressed };
    if (InstallEventHandler(GetEventDispatcherTarget(), PopupHotKeyHandler,
                            1, &spec, nullptr, &g_hotKeyHandler) != noErr) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
                     "Popup hotkey event handler installation failed; the menu item still works.");
        return;
    }

    // Primary: ⌘Insert — kVK_Help is the PC-Insert position (external
    // keyboards); most Apple laptops lack the key, hence the secondary.
    EventHotKeyID primaryId{ 'CLPP', 1 };
    if (RegisterEventHotKey(kVK_Help, cmdKey, primaryId,
                            GetEventDispatcherTarget(), 0, &g_hotKeyPrimary) == noErr) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Popup hotkey registered: Cmd+Insert.");
    } else {
        g_hotKeyPrimary = nullptr;
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Cmd+Insert unavailable.");
    }

    // Secondary: ⌃⌘V — typable on every keyboard.
    EventHotKeyID secondaryId{ 'CLPP', 2 };
    if (RegisterEventHotKey(kVK_ANSI_V, cmdKey | controlKey, secondaryId,
                            GetEventDispatcherTarget(), 0, &g_hotKeySecondary) == noErr) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Popup hotkey registered: Ctrl+Cmd+V.");
    } else {
        g_hotKeySecondary = nullptr;
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
                     "Ctrl+Cmd+V unavailable; the menu item still opens the popup.");
    }
}

void TogglePopupPanel() {
    if (g_popupController == nil) {
        g_popupController = [[ClippPopupController alloc] init];
    }
    [g_popupController toggle];
}

void DestroyPopupPanel() {
    if (g_hotKeyPrimary != nullptr) {
        UnregisterEventHotKey(g_hotKeyPrimary);
        g_hotKeyPrimary = nullptr;
    }
    if (g_hotKeySecondary != nullptr) {
        UnregisterEventHotKey(g_hotKeySecondary);
        g_hotKeySecondary = nullptr;
    }
    if (g_hotKeyHandler != nullptr) {
        RemoveEventHandler(g_hotKeyHandler);
        g_hotKeyHandler = nullptr;
    }
    if (g_popupController != nil) {
        [g_popupController destroy];
        g_popupController = nil;
    }
}

}  // namespace clipp

#endif  // __APPLE__
