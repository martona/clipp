#include "ClippPage.h"

#ifdef __APPLE__

#include "Clipboard.h"
#include "KeyManager.h"
#include "platform/uistrings.h"
#include "UiHelpers.h"

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <ctime>
#include <string>
#include <utility>

#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

extern KeyManager g_keyManager;

@interface MacOSClippPageTarget : NSObject {
@private
    MacOSClippPage* owner_;
    uint64_t itemID_;
}
- (instancetype)initWithOwner:(MacOSClippPage*)owner itemID:(uint64_t)itemID;
- (void)copyActivityItem:(id)sender;
- (void)showNetworkPage:(id)sender;
@end

@implementation MacOSClippPageTarget

- (instancetype)initWithOwner:(MacOSClippPage*)owner itemID:(uint64_t)itemID {
    self = [super init];
    if (self) {
        owner_ = owner;
        itemID_ = itemID;
    }
    return self;
}

- (void)copyActivityItem:(id)sender {
    (void)sender;
    if (owner_ != nullptr) {
        owner_->CopyActivityItem(itemID_);
    }
}

- (void)showNetworkPage:(id)sender {
    (void)sender;
    if (owner_ != nullptr) {
        owner_->ShowNetworkPage();
    }
}

@end

@interface MacOSActivityFlippedView : NSView
@end

@implementation MacOSActivityFlippedView

- (BOOL)isFlipped {
    return YES;
}

@end

namespace {
constexpr CGFloat kPageInset = 28.0;
constexpr CGFloat kActivityFollowBottomTolerance = 48.0;
constexpr CGFloat kActivityBubbleMaxWidth = 460.0;

NSString* FormatActivityTime(std::chrono::system_clock::time_point timestamp) {
    const std::time_t rawTime = std::chrono::system_clock::to_time_t(timestamp);
    std::tm localTime{};
    if (localtime_safe(&localTime, &rawTime) != 0) {
        return @"";
    }

    wchar_t buffer[32]{};
    if (std::wcsftime(buffer, cntof(buffer), L"%H:%M", &localTime) == 0) {
        return @"";
    }
    return MacOSToNSString(buffer);
}

NSString* PayloadKindLabel(ClipboardActivityPayloadKind kind) {
    switch (kind) {
    case ClipboardActivityPayloadKind::Text:
        return CLP_NS(CLP_UI_TEXT);
    case ClipboardActivityPayloadKind::PrivateText:
        return CLP_NS(CLP_UI_PRIVATE_TEXT);
    case ClipboardActivityPayloadKind::Link:
        return CLP_NS(CLP_UI_LINK);
    case ClipboardActivityPayloadKind::Image:
        return CLP_NS(CLP_UI_IMAGE);
    case ClipboardActivityPayloadKind::Unsupported:
    default:
        return CLP_NS(CLP_UI_UNSUPPORTED_CLIPBOARD_ITEM);
    }
}

NSColor* ActivityBubbleColor(bool isOutgoing) {
    if (isOutgoing) {
        return [NSColor colorWithCalibratedRed:0.0 green:0.45 blue:0.75 alpha:0.28];
    }
    return [NSColor alternatingContentBackgroundColors][1];
}

NSTextField* MakeActivityLabel(NSString* text, CGFloat fontSize, NSColor* color) {
    NSTextField* label = MacOSMakeWrappingLabel(text, fontSize, color);
    label.selectable = NO;
    return label;
}

NSTextField* MakeActivityPreviewLabel(NSString* text, bool selectable) {
    NSTextField* label = MakeActivityLabel(text, 13.0, [NSColor labelColor]);
    label.selectable = selectable;
    label.maximumNumberOfLines = 8;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    return label;
}
}

struct MacOSClippPageState {
    std::atomic_bool alive{ true };
};

MacOSClippPage::MacOSClippPage(ClipboardActivityStore& activityStore, NavigateCallback showNetworkPage)
    : activityStore_(activityStore)
    , showNetworkPage_(std::move(showNetworkPage))
    , pageState_(std::make_shared<MacOSClippPageState>()) {
    BuildView();
    RefreshActivityItems(activityStore_.Snapshot());
}

MacOSClippPage::~MacOSClippPage() {
    OnDestroy();
}

NSView* MacOSClippPage::View() const {
    return root_;
}

void MacOSClippPage::BuildView() {
    root_ = [[NSView alloc] initWithFrame:NSZeroRect];
    root_.translatesAutoresizingMaskIntoConstraints = NO;

    activityItemTargets_ = [[NSMutableArray alloc] init];
    actionTarget_ = [[MacOSClippPageTarget alloc] initWithOwner:this itemID:0];

    activityScroll_ = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    activityScroll_.translatesAutoresizingMaskIntoConstraints = NO;
    activityScroll_.hasVerticalScroller = YES;
    activityScroll_.hasHorizontalScroller = NO;
    activityScroll_.autohidesScrollers = YES;
    activityScroll_.borderType = NSNoBorder;
    activityScroll_.drawsBackground = NO;

    NSView* documentView = [[MacOSActivityFlippedView alloc] initWithFrame:NSZeroRect];
    documentView.translatesAutoresizingMaskIntoConstraints = NO;
    activityScroll_.documentView = documentView;

    activityItemsPanel_ = [[NSStackView alloc] initWithFrame:NSZeroRect];
    activityItemsPanel_.translatesAutoresizingMaskIntoConstraints = NO;
    activityItemsPanel_.orientation = NSUserInterfaceLayoutOrientationVertical;
    activityItemsPanel_.alignment = NSLayoutAttributeWidth;
    activityItemsPanel_.distribution = NSStackViewDistributionFill;
    activityItemsPanel_.spacing = 12.0;
    activityItemsPanel_.detachesHiddenViews = YES;

    activityEmptyState_ = [[NSStackView alloc] initWithFrame:NSZeroRect];
    activityEmptyState_.translatesAutoresizingMaskIntoConstraints = NO;
    activityEmptyState_.orientation = NSUserInterfaceLayoutOrientationVertical;
    activityEmptyState_.alignment = NSLayoutAttributeCenterX;
    activityEmptyState_.spacing = 10.0;

    activityEmptyMessage_ = MakeActivityLabel(@"", 14.0, [NSColor secondaryLabelColor]);
    activityEmptyMessage_.alignment = NSTextAlignmentCenter;

    activityEmptyNetworkButton_ = [NSButton buttonWithTitle:CLP_NS(CLP_UI_NETWORK)
                                                     target:actionTarget_
                                                     action:@selector(showNetworkPage:)];
    activityEmptyNetworkButton_.translatesAutoresizingMaskIntoConstraints = NO;
    activityEmptyNetworkButton_.bezelStyle = NSBezelStyleRounded;

    [activityEmptyState_ addArrangedSubview:activityEmptyMessage_];
    [activityEmptyState_ addArrangedSubview:activityEmptyNetworkButton_];

    [documentView addSubview:activityItemsPanel_];
    [root_ addSubview:activityScroll_];
    [root_ addSubview:activityEmptyState_];

    [NSLayoutConstraint activateConstraints:@[
        [activityScroll_.leadingAnchor constraintEqualToAnchor:root_.leadingAnchor],
        [activityScroll_.trailingAnchor constraintEqualToAnchor:root_.trailingAnchor],
        [activityScroll_.topAnchor constraintEqualToAnchor:root_.topAnchor],
        [activityScroll_.bottomAnchor constraintEqualToAnchor:root_.bottomAnchor],

        [documentView.widthAnchor constraintEqualToAnchor:activityScroll_.contentView.widthAnchor],

        [activityItemsPanel_.leadingAnchor constraintEqualToAnchor:documentView.leadingAnchor constant:kPageInset],
        [activityItemsPanel_.trailingAnchor constraintEqualToAnchor:documentView.trailingAnchor constant:-kPageInset],
        [activityItemsPanel_.topAnchor constraintEqualToAnchor:documentView.topAnchor constant:16.0],
        [activityItemsPanel_.bottomAnchor constraintEqualToAnchor:documentView.bottomAnchor constant:-kPageInset],

        [activityEmptyState_.centerXAnchor constraintEqualToAnchor:root_.centerXAnchor],
        [activityEmptyState_.centerYAnchor constraintEqualToAnchor:root_.centerYAnchor],
        [activityEmptyState_.leadingAnchor constraintGreaterThanOrEqualToAnchor:root_.leadingAnchor constant:kPageInset],
        [activityEmptyState_.trailingAnchor constraintLessThanOrEqualToAnchor:root_.trailingAnchor constant:-kPageInset],
    ]];

    UpdateActivityEmptyState();
}

NSView* MacOSClippPage::BuildActivityRow(uint64_t itemID) {
    const auto display = activityStore_.DisplayItem(itemID);
    if (!display) {
        return nil;
    }

    const bool isOutgoing = display->header.direction == ClipboardActivityDirection::Outgoing;

    NSView* row = [[NSView alloc] initWithFrame:NSZeroRect];
    row.translatesAutoresizingMaskIntoConstraints = NO;

    NSBox* bubble = MacOSMakeGroupBox();
    bubble.fillColor = ActivityBubbleColor(isOutgoing);
    bubble.cornerRadius = 8.0;

    NSStackView* content = [[NSStackView alloc] initWithFrame:NSZeroRect];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    content.orientation = NSUserInterfaceLayoutOrientationVertical;
    content.alignment = NSLayoutAttributeWidth;
    content.distribution = NSStackViewDistributionFill;
    content.spacing = 7.0;

    std::wstring deviceName = display->header.deviceName;
    if (deviceName.empty()) {
        deviceName = isOutgoing ? CLP_W(CLP_UI_THIS_DEVICE) : CLP_W(CLP_UI_UNKNOWN_HOST);
    }

    NSString* metaText = [NSString stringWithFormat:@"%@ - %@",
        MacOSToNSString(deviceName),
        FormatActivityTime(display->header.timestamp)];
    NSTextField* meta = MakeActivityLabel(metaText, 12.0, [NSColor secondaryLabelColor]);
    [content addArrangedSubview:meta];

    if (display->kind == ClipboardActivityPayloadKind::Image && !display->imageData.empty()) {
        NSData* data = [NSData dataWithBytes:display->imageData.data()
                                      length:display->imageData.size()];
        NSImage* image = [[NSImage alloc] initWithData:data];
        if (image != nil) {
            NSImageView* imageView = [[NSImageView alloc] initWithFrame:NSZeroRect];
            imageView.translatesAutoresizingMaskIntoConstraints = NO;
            imageView.image = image;
            imageView.imageScaling = NSImageScaleProportionallyUpOrDown;
            [imageView.widthAnchor constraintLessThanOrEqualToConstant:kActivityBubbleMaxWidth - 24.0].active = YES;
            [imageView.heightAnchor constraintLessThanOrEqualToConstant:260.0].active = YES;
            [content addArrangedSubview:imageView];
        }

        NSTextField* kindLabel = MakeActivityLabel(PayloadKindLabel(display->kind), 12.0, [NSColor secondaryLabelColor]);
        [content addArrangedSubview:kindLabel];
    } else {
        if (display->kind == ClipboardActivityPayloadKind::Link && !display->linkHost.empty()) {
            NSTextField* host = MakeActivityLabel(MacOSToNSString(display->linkHost), 13.0, [NSColor labelColor]);
            host.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightSemibold];
            [content addArrangedSubview:host];
        } else if (display->kind == ClipboardActivityPayloadKind::PrivateText ||
                   display->kind == ClipboardActivityPayloadKind::Unsupported) {
            NSTextField* kindLabel = MakeActivityLabel(PayloadKindLabel(display->kind), 13.0, [NSColor labelColor]);
            kindLabel.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightSemibold];
            [content addArrangedSubview:kindLabel];
        }

        NSString* previewText = display->previewText.empty()
            ? PayloadKindLabel(display->kind)
            : MacOSToNSString(display->previewText);
        NSTextField* preview = MakeActivityPreviewLabel(
            previewText,
            display->kind != ClipboardActivityPayloadKind::PrivateText);
        [content addArrangedSubview:preview];
    }

    MacOSClippPageTarget* target = [[MacOSClippPageTarget alloc] initWithOwner:this itemID:itemID];
    [activityItemTargets_ addObject:target];

    NSButton* copyButton = [NSButton buttonWithTitle:CLP_NS(CLP_UI_COPY)
                                             target:target
                                             action:@selector(copyActivityItem:)];
    copyButton.translatesAutoresizingMaskIntoConstraints = NO;
    copyButton.bezelStyle = NSBezelStyleRounded;
    [copyButton setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSView* buttonRow = [[NSView alloc] initWithFrame:NSZeroRect];
    buttonRow.translatesAutoresizingMaskIntoConstraints = NO;
    [buttonRow addSubview:copyButton];
    [NSLayoutConstraint activateConstraints:@[
        [copyButton.trailingAnchor constraintEqualToAnchor:buttonRow.trailingAnchor],
        [copyButton.topAnchor constraintEqualToAnchor:buttonRow.topAnchor],
        [copyButton.bottomAnchor constraintEqualToAnchor:buttonRow.bottomAnchor],
        [buttonRow.leadingAnchor constraintLessThanOrEqualToAnchor:copyButton.leadingAnchor],
    ]];
    [content addArrangedSubview:buttonRow];

    [bubble addSubview:content];
    [row addSubview:bubble];

    [NSLayoutConstraint activateConstraints:@[
        [bubble.topAnchor constraintEqualToAnchor:row.topAnchor],
        [bubble.bottomAnchor constraintEqualToAnchor:row.bottomAnchor],
        [bubble.widthAnchor constraintLessThanOrEqualToConstant:kActivityBubbleMaxWidth],
        isOutgoing
            ? [bubble.trailingAnchor constraintEqualToAnchor:row.trailingAnchor]
            : [bubble.leadingAnchor constraintEqualToAnchor:row.leadingAnchor],

        [content.leadingAnchor constraintEqualToAnchor:bubble.leadingAnchor constant:12.0],
        [content.trailingAnchor constraintEqualToAnchor:bubble.trailingAnchor constant:-12.0],
        [content.topAnchor constraintEqualToAnchor:bubble.topAnchor constant:10.0],
        [content.bottomAnchor constraintEqualToAnchor:bubble.bottomAnchor constant:-10.0],
    ]];

    return row;
}

void MacOSClippPage::RefreshActivityItems(const std::vector<ClipboardActivityItemHeader>& items) {
    if (activityItemsPanel_ == nil) {
        return;
    }

    for (NSView* view in activityItemsPanel_.arrangedSubviews) {
        [activityItemsPanel_ removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    [activityItemTargets_ removeAllObjects];
    activityItemIDs_.clear();

    for (const auto& item : items) {
        NSView* row = BuildActivityRow(item.id);
        if (row == nil) {
            continue;
        }
        [activityItemsPanel_ addArrangedSubview:row];
        activityItemIDs_.push_back(item.id);
    }

    SetActivityEmptyMessageVisible(activityItemIDs_.empty());
    ScrollActivityToBottom();
}

void MacOSClippPage::AddActivityItem(uint64_t itemID) {
    if (activityItemsPanel_ == nil || itemID == 0) {
        return;
    }

    if (std::find(activityItemIDs_.begin(), activityItemIDs_.end(), itemID) != activityItemIDs_.end()) {
        return;
    }

    const bool shouldFollow = IsActivityNearBottom();
    NSView* row = BuildActivityRow(itemID);
    if (row == nil) {
        return;
    }

    [activityItemsPanel_ addArrangedSubview:row];
    activityItemIDs_.push_back(itemID);
    SetActivityEmptyMessageVisible(false);

    if (shouldFollow) {
        ScrollActivityToBottom();
    }
}

void MacOSClippPage::RemoveActivityItem(uint64_t itemID) {
    if (activityItemsPanel_ == nil) {
        return;
    }

    const auto found = std::find(activityItemIDs_.begin(), activityItemIDs_.end(), itemID);
    if (found == activityItemIDs_.end()) {
        return;
    }

    const NSUInteger index = static_cast<NSUInteger>(found - activityItemIDs_.begin());
    NSArray<NSView*>* rows = activityItemsPanel_.arrangedSubviews;
    if (index < rows.count) {
        NSView* row = rows[index];
        [activityItemsPanel_ removeArrangedSubview:row];
        [row removeFromSuperview];
    }
    if (index < activityItemTargets_.count) {
        [activityItemTargets_ removeObjectAtIndex:index];
    }
    activityItemIDs_.erase(found);
    SetActivityEmptyMessageVisible(activityItemIDs_.empty());
}

void MacOSClippPage::ClearActivityItems() {
    if (activityItemsPanel_ == nil) {
        return;
    }

    for (NSView* view in activityItemsPanel_.arrangedSubviews) {
        [activityItemsPanel_ removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    [activityItemTargets_ removeAllObjects];
    activityItemIDs_.clear();
    SetActivityEmptyMessageVisible(true);
}

void MacOSClippPage::SetActivityEmptyMessageVisible(bool visible) {
    if (activityEmptyState_ == nil) {
        return;
    }

    UpdateActivityEmptyState();
    activityEmptyState_.hidden = !visible;
}

void MacOSClippPage::UpdateActivityEmptyState() {
    if (activityEmptyMessage_ == nil || activityEmptyNetworkButton_ == nil) {
        return;
    }

    const bool haveNetworkKey = g_keyManager.HaveNetworkKey();
    MacOSSetFieldText(activityEmptyMessage_, haveNetworkKey
        ? CLP_NS(CLP_UI_CLIPBOARD_EMPTY)
        : CLP_NS(CLP_UI_NO_NETWORK_KEY_CONFIGURED));
    activityEmptyNetworkButton_.hidden = haveNetworkKey;
}

bool MacOSClippPage::IsActivityNearBottom() const {
    if (activityScroll_ == nil || activityScroll_.documentView == nil) {
        return true;
    }

    [activityScroll_ layoutSubtreeIfNeeded];
    [activityScroll_.documentView layoutSubtreeIfNeeded];

    const NSRect visibleRect = activityScroll_.contentView.documentVisibleRect;
    const CGFloat documentHeight = activityScroll_.documentView.bounds.size.height;
    if (documentHeight <= visibleRect.size.height) {
        return true;
    }

    return (documentHeight - NSMaxY(visibleRect)) <= kActivityFollowBottomTolerance;
}

void MacOSClippPage::ScrollActivityToBottom() const {
    if (activityScroll_ == nil || activityScroll_.documentView == nil) {
        return;
    }

    [activityScroll_ layoutSubtreeIfNeeded];
    [activityScroll_.documentView layoutSubtreeIfNeeded];

    const CGFloat documentHeight = activityScroll_.documentView.bounds.size.height;
    const CGFloat visibleHeight = activityScroll_.contentView.bounds.size.height;
    const CGFloat y = (std::max)(static_cast<CGFloat>(0.0), documentHeight - visibleHeight);
    [activityScroll_.contentView scrollToPoint:NSMakePoint(0.0, y)];
    [activityScroll_ reflectScrolledClipView:activityScroll_.contentView];
}

void MacOSClippPage::CopyActivityItem(uint64_t itemID) {
    auto payload = activityStore_.PayloadForClipboard(itemID);
    if (!payload) {
        return;
    }

    SetClipboardData(*payload);
}

void MacOSClippPage::ShowNetworkPage() {
    if (showNetworkPage_) {
        showNetworkPage_();
    }
}

void MacOSClippPage::OnShown() {
    if (destroyed_) {
        return;
    }

    BeginActivityNotifications();
    auto state = pageState_;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (state->alive.load()) {
            this->UpdateActivityEmptyState();
            this->RefreshActivityItems(this->activityStore_.Snapshot());
        }
    });
}

void MacOSClippPage::OnHidden() {
    EndActivityNotifications();
}

void MacOSClippPage::OnDestroy() {
    if (destroyed_) {
        return;
    }

    destroyed_ = true;
    if (pageState_) {
        pageState_->alive.store(false);
    }
    OnHidden();
    [activityItemTargets_ removeAllObjects];
    actionTarget_ = nil;
    root_ = nullptr;
    activityScroll_ = nullptr;
    activityItemsPanel_ = nullptr;
    activityEmptyState_ = nullptr;
    activityEmptyMessage_ = nullptr;
    activityEmptyNetworkButton_ = nullptr;
}

void MacOSClippPage::OnNetworkKeyChanged() {
    auto state = pageState_;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (state->alive.load()) {
            this->UpdateActivityEmptyState();
        }
    });
}

NSView* MacOSClippPage::FirstKeyView() const {
    if (activityEmptyNetworkButton_ != nil && !activityEmptyNetworkButton_.hidden) {
        return activityEmptyNetworkButton_;
    }
    return nil;
}

void MacOSClippPage::ConnectKeyViewLoop(NSView* nextKeyView) {
    nextKeyViewAfterPage_ = nextKeyView;
    if (activityEmptyNetworkButton_ != nil) {
        activityEmptyNetworkButton_.nextKeyView = nextKeyView;
    }
}

void MacOSClippPage::BeginActivityNotifications() {
    if (activityWatcherID_ != 0 || destroyed_) {
        return;
    }

    const auto registration = activityStore_.QueryAndRegister(ClipboardActivityWatcher, this);
    activityWatcherID_ = registration.watcherID;
    RefreshActivityItems(registration.items);
}

void MacOSClippPage::EndActivityNotifications() {
    if (activityWatcherID_ == 0) {
        return;
    }

    activityStore_.Unregister(activityWatcherID_);
    activityWatcherID_ = 0;
}

void MacOSClippPage::ClipboardActivityWatcher(const ClipboardActivityUpdate& update, void* userData) {
    auto* page = reinterpret_cast<MacOSClippPage*>(userData);
    if (page == nullptr || !page->pageState_) {
        return;
    }

    ClipboardActivityUpdate updateCopy = update;
    auto state = page->pageState_;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!state->alive.load()) {
            return;
        }

        switch (updateCopy.type) {
        case ClipboardActivityUpdate::Type::Added:
            page->AddActivityItem(updateCopy.itemID);
            break;
        case ClipboardActivityUpdate::Type::Removed:
            page->RemoveActivityItem(updateCopy.itemID);
            break;
        case ClipboardActivityUpdate::Type::Cleared:
            page->ClearActivityItems();
            break;
        }
    });
}

#endif
