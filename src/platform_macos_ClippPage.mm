#include "platform_macos_ClippPage.h"

#ifdef __APPLE__

#include "KeyManager.h"
#include "MDNSThread.h"
#include "PeerManager.h"
#include "Settings.h"
#include "platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sodium.h>

#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

extern Settings g_settings;
extern KeyManager g_keyManager;
extern PeerDisplay g_peerDisplay;
extern PeerManager g_peerManager;

class MacOSNetworkItemView;

@interface MacOSClippPageFieldDelegate : NSObject <NSTextFieldDelegate> {
@private
    MacOSClippPage* owner_;
}
- (instancetype)initWithOwner:(MacOSClippPage*)owner;
@end

@implementation MacOSClippPageFieldDelegate

- (instancetype)initWithOwner:(MacOSClippPage*)owner {
    self = [super init];
    if (self) {
        owner_ = owner;
    }
    return self;
}

- (void)controlTextDidBeginEditing:(NSNotification*)notification {
    if (owner_ == nullptr || ![notification.object isKindOfClass:[NSTextField class]]) {
        return;
    }

    owner_->OnFieldEditingBegan(static_cast<NSTextField*>(notification.object));
}

- (void)controlTextDidChange:(NSNotification*)notification {
    if (owner_ == nullptr || ![notification.object isKindOfClass:[NSTextField class]]) {
        return;
    }

    owner_->OnFieldEditingChanged(static_cast<NSTextField*>(notification.object));
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    if (owner_ == nullptr || ![notification.object isKindOfClass:[NSTextField class]]) {
        return;
    }

    owner_->OnFieldEditingEnded(static_cast<NSTextField*>(notification.object));
}

@end

@interface MacOSFlippedView : NSView
@end

@implementation MacOSFlippedView

- (BOOL)isFlipped {
    return YES;
}

@end

@interface MacOSNetworkItemTarget : NSObject {
@private
    MacOSNetworkItemView* owner_;
}
- (instancetype)initWithOwner:(MacOSNetworkItemView*)owner;
- (void)toggleDisclosure:(id)sender;
@end

namespace {
constexpr CGFloat kPageInset = 28.0;
constexpr CGFloat kSectionInsetX = 18.0;
constexpr CGFloat kSectionInsetY = 14.0;
constexpr char kMaskedPassword[] = "****************";
constexpr uint64_t kMaxByteCounter = 999'999'999'999;

NSString* ToNSString(const std::string& value) {
    NSString* text = [NSString stringWithUTF8String:value.c_str()];
    return text != nil ? text : @"";
}

NSString* ToNSString(const std::wstring& value) {
    const size_t utf8Len = utf16_to_utf8(value.c_str(), value.size(), nullptr, 0);
    if (utf8Len == 0) {
        return @"";
    }

    std::string utf8;
    utf8.resize(utf8Len);
    utf16_to_utf8(value.c_str(), value.size(), utf8.data(), utf8.size());
    return ToNSString(utf8);
}

std::string ToStdString(NSString* value) {
    if (value == nil) {
        return {};
    }

    const char* utf8 = value.UTF8String;
    return utf8 != nullptr ? std::string(utf8) : std::string();
}

NSTextField* MakeLabel(NSString* text) {
    NSTextField* label = [NSTextField labelWithString:text];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.font = [NSFont systemFontOfSize:13];
    label.textColor = [NSColor labelColor];
    return label;
}

NSTextField* MakeWrappingLabel(NSString* text, CGFloat fontSize, NSColor* color) {
    NSTextField* label = [NSTextField wrappingLabelWithString:text];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.font = [NSFont systemFontOfSize:fontSize];
    label.textColor = color;
    return label;
}

NSTextField* MakeTextField(CGFloat width) {
    NSTextField* field = [NSTextField textFieldWithString:@""];
    field.translatesAutoresizingMaskIntoConstraints = NO;
    field.font = [NSFont systemFontOfSize:13];
    field.alignment = NSTextAlignmentLeft;
    [field.widthAnchor constraintGreaterThanOrEqualToConstant:width].active = YES;
    return field;
}

NSSecureTextField* MakeSecureTextField(CGFloat width) {
    NSSecureTextField* field = [[NSSecureTextField alloc] initWithFrame:NSZeroRect];
    field.translatesAutoresizingMaskIntoConstraints = NO;
    field.font = [NSFont systemFontOfSize:13];
    field.alignment = NSTextAlignmentLeft;
    field.contentType = NSTextContentTypePassword;
    [field.widthAnchor constraintGreaterThanOrEqualToConstant:width].active = YES;
    [field.heightAnchor constraintEqualToConstant:22.0].active = YES;
    return field;
}

NSBox* MakeGroupBox() {
    NSBox* box = [[NSBox alloc] initWithFrame:NSZeroRect];
    box.translatesAutoresizingMaskIntoConstraints = NO;
    box.boxType = NSBoxCustom;
    box.titlePosition = NSNoTitle;
    box.borderType = NSNoBorder;
    box.cornerRadius = 10.0;
    box.fillColor = [NSColor alternatingContentBackgroundColors][1];
    return box;
}

NSImage* MakeSymbolImage(NSString* symbolName, NSString* accessibilityDescription, CGFloat pointSize, NSColor* tintColor) {
    NSImage* image = [NSImage imageWithSystemSymbolName:symbolName accessibilityDescription:accessibilityDescription];
    if (image == nil) {
        return nil;
    }

    NSImageSymbolConfiguration* config = [NSImageSymbolConfiguration configurationWithPointSize:pointSize weight:NSFontWeightMedium];
    image = [image imageWithSymbolConfiguration:config];
    [image setTemplate:YES];
    (void)tintColor;
    return image;
}

NSImageView* MakeSymbolImageView(NSString* symbolName, NSString* accessibilityDescription, NSColor* tintColor) {
    NSImageView* imageView = [[NSImageView alloc] initWithFrame:NSZeroRect];
    imageView.translatesAutoresizingMaskIntoConstraints = NO;
    imageView.image = MakeSymbolImage(symbolName, accessibilityDescription, 16.0, tintColor);
    imageView.contentTintColor = tintColor;
    imageView.imageScaling = NSImageScaleProportionallyDown;
    [imageView.widthAnchor constraintEqualToConstant:22.0].active = YES;
    [imageView.heightAnchor constraintEqualToConstant:22.0].active = YES;
    return imageView;
}

NSButton* MakeIconButton(NSString* symbolName, NSString* accessibilityDescription, id target, SEL action) {
    NSButton* button = [NSButton buttonWithImage:MakeSymbolImage(symbolName, accessibilityDescription, 13.0, [NSColor secondaryLabelColor])
                                          target:target
                                          action:action];
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.bezelStyle = NSBezelStyleRegularSquare;
    button.bordered = NO;
    button.imagePosition = NSImageOnly;
    button.contentTintColor = [NSColor secondaryLabelColor];
    button.toolTip = accessibilityDescription;
    [button.widthAnchor constraintEqualToConstant:28.0].active = YES;
    [button.heightAnchor constraintEqualToConstant:28.0].active = YES;
    return button;
}

void AddInputRow(
    NSView* section,
    NSTextField* label,
    NSView* field,
    NSView* previousField,
    NSMutableArray<NSLayoutConstraint*>* constraints)
{
    [section addSubview:label];
    [section addSubview:field];

    [constraints addObjectsFromArray:@[
        [label.leadingAnchor constraintEqualToAnchor:section.leadingAnchor constant:kSectionInsetX],
        [label.widthAnchor constraintEqualToConstant:115.0],
        [label.centerYAnchor constraintEqualToAnchor:field.centerYAnchor],

        [field.leadingAnchor constraintEqualToAnchor:label.trailingAnchor constant:16.0],
        [field.trailingAnchor constraintLessThanOrEqualToAnchor:section.trailingAnchor constant:-kSectionInsetX],
    ]];

    if (previousField == nil) {
        [constraints addObject:[field.topAnchor constraintEqualToAnchor:section.topAnchor constant:kSectionInsetY]];
    } else {
        [constraints addObject:[field.topAnchor constraintEqualToAnchor:previousField.bottomAnchor constant:12.0]];
    }
}

void SetFieldText(NSTextField* field, NSString* value) {
    if (field == nil) {
        return;
    }

    field.stringValue = value != nil ? value : @"";
}

NSString* DisplayHostName(const std::wstring& hostName) {
    return hostName.empty() ? @"(unknown host)" : ToNSString(hostName);
}

NSString* FormatByteCounter(uint64_t bytes) {
    if (bytes > kMaxByteCounter) {
        return @"+++,+++,+++,+++";
    }

    std::string digits = std::to_string(bytes);
    std::string counter;
    counter.reserve(digits.size() + ((digits.size() - 1) / 3));
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && ((digits.size() - i) % 3) == 0) {
            counter.push_back(',');
        }
        counter.push_back(digits[i]);
    }

    return ToNSString(counter);
}

NSString* FormatConnectionState(bool connected) {
    return connected ? @"Connected" : @"Not connected";
}

std::string FormatConnectedFor(std::chrono::steady_clock::time_point connectedSince, std::chrono::steady_clock::time_point now) {
    if (connectedSince == std::chrono::steady_clock::time_point{}) {
        return "Not connected";
    }

    if (now < connectedSince) {
        now = connectedSince;
    }

    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - connectedSince).count();
    const auto days = seconds / (60 * 60 * 24);
    seconds %= (60 * 60 * 24);
    const auto hours = seconds / (60 * 60);
    seconds %= (60 * 60);
    const auto minutes = seconds / 60;
    seconds %= 60;

    char timeBuffer[32]{};
    std::snprintf(timeBuffer,
                  sizeof(timeBuffer),
                  "%02lld:%02lld:%02lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds));

    std::string text = "Connected for ";
    if (days > 0) {
        text += std::to_string(days);
        text += days == 1 ? " day, " : " days, ";
    }
    text += timeBuffer;
    return text;
}

bool IsMaskedPasswordPlaceholder(NSTextField* field) {
    return field != nil && ToStdString(field.stringValue) == kMaskedPassword;
}
}

struct MacOSClippPageState {
    std::atomic_bool alive{ true };
};

class MacOSKeyDerivationWorker {
public:
    using ResultHandler = std::function<void(const KeyManager::NetworkKey&)>;

    explicit MacOSKeyDerivationWorker(ResultHandler resultHandler)
        : resultHandler_(std::move(resultHandler))
        , workerThread_(&MacOSKeyDerivationWorker::WorkerLoop, this) {
    }

    ~MacOSKeyDerivationWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true;
            sodium_memzero(pendingPassword_.data(), pendingPassword_.capacity());
            pendingPassword_.clear();
        }
        cv_.notify_one();
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    }

    void RequestKeyDerivation(const std::string& password) {
        std::lock_guard<std::mutex> lock(mutex_);
        sodium_memzero(pendingPassword_.data(), pendingPassword_.capacity());
        pendingPassword_ = password;
        ++currentGeneration_;
        hasPendingWork_ = true;
        cv_.notify_one();
    }

private:
    void WorkerLoop() {
        while (true) {
            std::string targetPassword;
            uint64_t targetGeneration = 0;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() {
                    return hasPendingWork_ || stopRequested_;
                });

                if (stopRequested_) {
                    break;
                }

                targetPassword = pendingPassword_;
                targetGeneration = currentGeneration_;
                sodium_memzero(pendingPassword_.data(), pendingPassword_.capacity());
                pendingPassword_.clear();
                hasPendingWork_ = false;
            }

            KeyManager::NetworkKey newKey{};
            const bool success = g_keyManager.DeriveNetworkKey(targetPassword, newKey);
            sodium_memzero(targetPassword.data(), targetPassword.capacity());

            bool shouldApply = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                shouldApply = success && targetGeneration == currentGeneration_ && !stopRequested_;
            }

            if (shouldApply && resultHandler_) {
                resultHandler_(newKey);
            }
            sodium_memzero(newKey.data(), newKey.size());
        }
    }

    ResultHandler resultHandler_;
    std::thread workerThread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopRequested_ = false;
    bool hasPendingWork_ = false;
    std::string pendingPassword_;
    uint64_t currentGeneration_ = 0;
};

class MacOSNetworkItemView {
public:
    explicit MacOSNetworkItemView(const PeerDisplayItem& item)
        : disclosureTarget_([[MacOSNetworkItemTarget alloc] initWithOwner:this]) {
        BuildView();
        UpdateHostName(item.hostName);
        UpdateIncomingConnection(item.hasIncomingConnection);
        UpdateOutgoingConnection(item.hasOutgoingConnection);
        UpdateBytesSent(item.bytesSent);
        UpdateBytesReceived(item.bytesReceived);
        UpdateConnectedSince(item.connectedSince);
    }

    NSView* View() const {
        return card_;
    }

    NSButton* DisclosureButton() const {
        return disclosureButton_;
    }

    void ToggleDisclosure() {
        detailsPanel_.hidden = !detailsPanel_.hidden;
        UpdateDisclosureImage();
        [card_ layoutSubtreeIfNeeded];
    }

    void UpdateHostName(const std::wstring& hostName) {
        SetFieldText(title_, DisplayHostName(hostName));
    }

    void UpdateHostID(const HostId&) {
    }

    void UpdateIncomingConnection(bool connected) {
        incomingIcon_.hidden = !connected;
        SetFieldText(incomingValue_, FormatConnectionState(connected));
    }

    void UpdateOutgoingConnection(bool connected) {
        outgoingIcon_.hidden = !connected;
        SetFieldText(outgoingValue_, FormatConnectionState(connected));
    }

    void UpdateBytesSent(uint64_t bytesSent) {
        SetFieldText(bytesSentValue_, FormatByteCounter(bytesSent));
    }

    void UpdateBytesReceived(uint64_t bytesReceived) {
        SetFieldText(bytesReceivedValue_, FormatByteCounter(bytesReceived));
    }

    void UpdateConnectedSince(std::chrono::steady_clock::time_point connectedSince) {
        connectedSince_ = connectedSince;
        connectedForText_.clear();
        RefreshConnectedFor();
    }

    void RefreshConnectedFor(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        const std::string text = FormatConnectedFor(connectedSince_, now);
        if (connectedForText_ != text) {
            connectedForText_ = text;
            SetFieldText(subtitle_, ToNSString(text));
        }
    }

private:
    NSTextField* AddDetailRow(NSGridView* grid, NSInteger rowIndex, NSString* labelText) {
        NSTextField* label = MakeLabel(labelText);
        label.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];

        NSTextField* value = MakeLabel(@"");
        value.textColor = [NSColor secondaryLabelColor];

        [grid addRowWithViews:@[label, value]];
        NSGridRow* row = [grid rowAtIndex:rowIndex];
        row.yPlacement = NSGridCellPlacementCenter;
        return value;
    }

    void BuildView() {
        card_ = MakeGroupBox();

        NSStackView* cardStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
        cardStack.translatesAutoresizingMaskIntoConstraints = NO;
        cardStack.orientation = NSUserInterfaceLayoutOrientationVertical;
        cardStack.alignment = NSLayoutAttributeLeading;
        cardStack.distribution = NSStackViewDistributionFill;
        cardStack.spacing = 0.0;
        cardStack.detachesHiddenViews = YES;

        NSView* headerRow = [[NSView alloc] initWithFrame:NSZeroRect];
        headerRow.translatesAutoresizingMaskIntoConstraints = NO;

        NSImageView* networkIcon = MakeSymbolImageView(@"network", @"Network", [NSColor secondaryLabelColor]);
        NSStackView* titleStack = CreateTitleStack();
        NSStackView* statusStack = CreateStatusStack();
        NSButton* disclosureButton = CreateDisclosureButton();

        [titleStack setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        [titleStack setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        [statusStack setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
        [statusStack setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

        [headerRow addSubview:networkIcon];
        [headerRow addSubview:titleStack];
        [headerRow addSubview:statusStack];
        [headerRow addSubview:disclosureButton];

        detailsPanel_ = [[NSView alloc] initWithFrame:NSZeroRect];
        detailsPanel_.translatesAutoresizingMaskIntoConstraints = NO;
        detailsPanel_.hidden = YES;

        NSGridView* detailsGrid = [[NSGridView alloc] initWithFrame:NSZeroRect];
        detailsGrid.translatesAutoresizingMaskIntoConstraints = NO;
        detailsGrid.columnSpacing = 24.0;
        detailsGrid.rowSpacing = 7.0;

        bytesSentValue_ = AddDetailRow(detailsGrid, 0, @"Bytes sent:");
        bytesReceivedValue_ = AddDetailRow(detailsGrid, 1, @"Bytes received:");
        incomingValue_ = AddDetailRow(detailsGrid, 2, @"Incoming:");
        outgoingValue_ = AddDetailRow(detailsGrid, 3, @"Outgoing:");
        [detailsGrid columnAtIndex:0].xPlacement = NSGridCellPlacementLeading;
        [detailsGrid columnAtIndex:1].xPlacement = NSGridCellPlacementLeading;

        [detailsPanel_ addSubview:detailsGrid];
        [cardStack addArrangedSubview:headerRow];
        [cardStack addArrangedSubview:detailsPanel_];
        [card_ addSubview:cardStack];

        [NSLayoutConstraint activateConstraints:@[
            [cardStack.leadingAnchor constraintEqualToAnchor:card_.leadingAnchor],
            [cardStack.trailingAnchor constraintEqualToAnchor:card_.trailingAnchor],
            [cardStack.topAnchor constraintEqualToAnchor:card_.topAnchor],
            [cardStack.bottomAnchor constraintEqualToAnchor:card_.bottomAnchor],

            [headerRow.widthAnchor constraintEqualToAnchor:cardStack.widthAnchor],
            [headerRow.heightAnchor constraintGreaterThanOrEqualToConstant:64.0],

            [networkIcon.leadingAnchor constraintEqualToAnchor:headerRow.leadingAnchor constant:16.0],
            [networkIcon.centerYAnchor constraintEqualToAnchor:headerRow.centerYAnchor],

            [titleStack.leadingAnchor constraintEqualToAnchor:networkIcon.trailingAnchor constant:12.0],
            [titleStack.centerYAnchor constraintEqualToAnchor:headerRow.centerYAnchor],
            [titleStack.topAnchor constraintGreaterThanOrEqualToAnchor:headerRow.topAnchor constant:12.0],
            [titleStack.bottomAnchor constraintLessThanOrEqualToAnchor:headerRow.bottomAnchor constant:-12.0],

            [statusStack.leadingAnchor constraintGreaterThanOrEqualToAnchor:titleStack.trailingAnchor constant:12.0],
            [statusStack.trailingAnchor constraintEqualToAnchor:disclosureButton.leadingAnchor constant:-10.0],
            [statusStack.centerYAnchor constraintEqualToAnchor:headerRow.centerYAnchor],

            [disclosureButton.trailingAnchor constraintEqualToAnchor:headerRow.trailingAnchor constant:-12.0],
            [disclosureButton.centerYAnchor constraintEqualToAnchor:headerRow.centerYAnchor],

            [detailsPanel_.widthAnchor constraintEqualToAnchor:cardStack.widthAnchor],

            [detailsGrid.leadingAnchor constraintEqualToAnchor:detailsPanel_.leadingAnchor constant:56.0],
            [detailsGrid.trailingAnchor constraintLessThanOrEqualToAnchor:detailsPanel_.trailingAnchor constant:-16.0],
            [detailsGrid.topAnchor constraintEqualToAnchor:detailsPanel_.topAnchor constant:6.0],
            [detailsGrid.bottomAnchor constraintEqualToAnchor:detailsPanel_.bottomAnchor constant:-16.0],
        ]];

        UpdateDisclosureImage();
    }

    NSStackView* CreateTitleStack() {
        NSStackView* textStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
        textStack.translatesAutoresizingMaskIntoConstraints = NO;
        textStack.orientation = NSUserInterfaceLayoutOrientationVertical;
        textStack.alignment = NSLayoutAttributeLeading;
        textStack.distribution = NSStackViewDistributionFill;
        textStack.spacing = 2.0;

        title_ = MakeLabel(@"");
        title_.font = [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];

        subtitle_ = MakeLabel(@"");
        subtitle_.font = [NSFont systemFontOfSize:12];
        subtitle_.textColor = [NSColor secondaryLabelColor];

        [textStack addArrangedSubview:title_];
        [textStack addArrangedSubview:subtitle_];
        return textStack;
    }

    NSStackView* CreateStatusStack() {
        NSStackView* statusStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
        statusStack.translatesAutoresizingMaskIntoConstraints = NO;
        statusStack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        statusStack.alignment = NSLayoutAttributeCenterY;
        statusStack.distribution = NSStackViewDistributionFill;
        statusStack.spacing = 4.0;

        incomingIcon_ = MakeSymbolImageView(@"arrow.down.circle.fill", @"Incoming connection", [NSColor systemGreenColor]);
        outgoingIcon_ = MakeSymbolImageView(@"arrow.up.circle.fill", @"Outgoing connection", [NSColor systemBlueColor]);

        [statusStack addArrangedSubview:incomingIcon_];
        [statusStack addArrangedSubview:outgoingIcon_];
        return statusStack;
    }

    NSButton* CreateDisclosureButton() {
        disclosureButton_ = MakeIconButton(@"chevron.right", @"Peer details", disclosureTarget_, @selector(toggleDisclosure:));
        return disclosureButton_;
    }

    void UpdateDisclosureImage() {
        NSString* symbol = detailsPanel_.hidden ? @"chevron.right" : @"chevron.down";
        disclosureButton_.image = MakeSymbolImage(symbol, @"Peer details", 13.0, [NSColor secondaryLabelColor]);
    }

    NSView* card_ = nullptr;
    NSTextField* title_ = nullptr;
    NSTextField* subtitle_ = nullptr;
    NSImageView* incomingIcon_ = nullptr;
    NSImageView* outgoingIcon_ = nullptr;
    NSButton* disclosureButton_ = nullptr;
    NSView* detailsPanel_ = nullptr;
    NSTextField* bytesSentValue_ = nullptr;
    NSTextField* bytesReceivedValue_ = nullptr;
    NSTextField* incomingValue_ = nullptr;
    NSTextField* outgoingValue_ = nullptr;
    MacOSNetworkItemTarget* disclosureTarget_ = nullptr;
    std::chrono::steady_clock::time_point connectedSince_{};
    std::string connectedForText_;
};

@implementation MacOSNetworkItemTarget

- (instancetype)initWithOwner:(MacOSNetworkItemView*)owner {
    self = [super init];
    if (self) {
        owner_ = owner;
    }
    return self;
}

- (void)toggleDisclosure:(id)sender {
    (void)sender;
    if (owner_ != nullptr) {
        owner_->ToggleDisclosure();
    }
}

@end

class MacOSNetworkView {
public:
    MacOSNetworkView(PeerDisplay& peerDisplay, std::function<void()> keyViewChangedHandler)
        : peerDisplay_(peerDisplay)
        , keyViewChangedHandler_(std::move(keyViewChangedHandler)) {
        BuildView();
    }

    NSView* View() const {
        return container_;
    }

    void Poll() {
        auto items = peerDisplay_.Query();
        std::sort(items.begin(), items.end(), ItemLess);

        const auto now = std::chrono::steady_clock::now();
        bool keyViewsChanged = false;

        for (std::size_t index = entries_.size(); index > 0; --index) {
            const auto entryIndex = index - 1;
            const auto found = std::find_if(items.begin(), items.end(), [&](const PeerDisplayItem& item) {
                return SameHostID(entries_[entryIndex].item, item);
            });
            if (found == items.end()) {
                NSView* view = entries_[entryIndex].view->View();
                [itemsPanel_ removeArrangedSubview:view];
                [view removeFromSuperview];
                entries_.erase(entries_.begin() + entryIndex);
                keyViewsChanged = true;
            }
        }

        for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
            const auto& item = items[itemIndex];
            const auto existingIndex = FindEntryByHostID(item);
            if (existingIndex == entries_.size()) {
                Entry entry;
                entry.item = item;
                entry.view = std::make_unique<MacOSNetworkItemView>(item);
                entry.view->RefreshConnectedFor(now);
                NSView* entryView = entry.view->View();
                [itemsPanel_ insertArrangedSubview:entryView atIndex:static_cast<NSUInteger>(itemIndex)];
                [entryView.widthAnchor constraintEqualToAnchor:itemsPanel_.widthAnchor].active = YES;
                entries_.insert(entries_.begin() + itemIndex, std::move(entry));
                keyViewsChanged = true;
                continue;
            }

            if (existingIndex != itemIndex) {
                auto entry = std::move(entries_[existingIndex]);
                entries_.erase(entries_.begin() + existingIndex);
                [itemsPanel_ removeArrangedSubview:entry.view->View()];
                [entry.view->View() removeFromSuperview];

                const auto insertIndex = itemIndex > entries_.size() ? entries_.size() : itemIndex;
                [itemsPanel_ insertArrangedSubview:entry.view->View() atIndex:static_cast<NSUInteger>(insertIndex)];
                entries_.insert(entries_.begin() + insertIndex, std::move(entry));
                keyViewsChanged = true;
            }

            UpdateEntry(entries_[itemIndex], item, now);
        }

        SetEmptyMessageVisible(entries_.empty());
        if (keyViewsChanged && keyViewChangedHandler_) {
            keyViewChangedHandler_();
        }
    }

    void AppendKeyViews(NSMutableArray<NSView*>* keyViews) const {
        for (const auto& entry : entries_) {
            NSButton* disclosureButton = entry.view->DisclosureButton();
            if (disclosureButton != nil) {
                [keyViews addObject:disclosureButton];
            }
        }
    }

private:
    struct Entry {
        PeerDisplayItem item;
        std::unique_ptr<MacOSNetworkItemView> view;
    };

    void BuildView() {
        container_ = [[NSStackView alloc] initWithFrame:NSZeroRect];
        container_.translatesAutoresizingMaskIntoConstraints = NO;
        container_.orientation = NSUserInterfaceLayoutOrientationVertical;
        container_.alignment = NSLayoutAttributeLeading;
        container_.distribution = NSStackViewDistributionFill;
        container_.spacing = 8.0;
        container_.detachesHiddenViews = YES;

        NSTextField* heading = [NSTextField labelWithString:@"Peers"];
        heading.translatesAutoresizingMaskIntoConstraints = NO;
        heading.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
        heading.textColor = [NSColor labelColor];

        emptyMessage_ = MakeWrappingLabel(@"Make sure your devices are using the exact same network name and secret. Both are case-sensitive.",
                                          13.0,
                                          [NSColor secondaryLabelColor]);

        itemsPanel_ = [[NSStackView alloc] initWithFrame:NSZeroRect];
        itemsPanel_.translatesAutoresizingMaskIntoConstraints = NO;
        itemsPanel_.orientation = NSUserInterfaceLayoutOrientationVertical;
        itemsPanel_.alignment = NSLayoutAttributeLeading;
        itemsPanel_.distribution = NSStackViewDistributionFill;
        itemsPanel_.spacing = 8.0;

        [container_ addArrangedSubview:heading];
        [container_ addArrangedSubview:emptyMessage_];
        [container_ addArrangedSubview:itemsPanel_];

        [heading.widthAnchor constraintLessThanOrEqualToAnchor:container_.widthAnchor].active = YES;
        [emptyMessage_.widthAnchor constraintEqualToAnchor:container_.widthAnchor].active = YES;
        [itemsPanel_.widthAnchor constraintEqualToAnchor:container_.widthAnchor].active = YES;
    }

    static bool ItemLess(const PeerDisplayItem& left, const PeerDisplayItem& right) {
        return left.hostID < right.hostID;
    }

    static bool SameHostID(const PeerDisplayItem& left, const PeerDisplayItem& right) {
        return left.hostID == right.hostID;
    }

    std::size_t FindEntryByHostID(const PeerDisplayItem& item) const {
        const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
            return SameHostID(entry.item, item);
        });
        return found == entries_.end() ? entries_.size() : static_cast<std::size_t>(found - entries_.begin());
    }

    void SetEmptyMessageVisible(bool visible) {
        emptyMessage_.hidden = !visible;
    }

    void UpdateEntry(Entry& entry, const PeerDisplayItem& item, std::chrono::steady_clock::time_point now) {
        if (entry.item.hostName != item.hostName) {
            entry.view->UpdateHostName(item.hostName);
        }
        if (entry.item.hostID != item.hostID) {
            entry.view->UpdateHostID(item.hostID);
        }
        if (entry.item.hasIncomingConnection != item.hasIncomingConnection) {
            entry.view->UpdateIncomingConnection(item.hasIncomingConnection);
        }
        if (entry.item.hasOutgoingConnection != item.hasOutgoingConnection) {
            entry.view->UpdateOutgoingConnection(item.hasOutgoingConnection);
        }
        if (entry.item.bytesSent != item.bytesSent) {
            entry.view->UpdateBytesSent(item.bytesSent);
        }
        if (entry.item.bytesReceived != item.bytesReceived) {
            entry.view->UpdateBytesReceived(item.bytesReceived);
        }
        if (entry.item.connectedSince != item.connectedSince) {
            entry.view->UpdateConnectedSince(item.connectedSince);
        } else {
            entry.view->RefreshConnectedFor(now);
        }

        entry.item = item;
    }

    PeerDisplay& peerDisplay_;
    std::function<void()> keyViewChangedHandler_;
    NSStackView* container_ = nullptr;
    NSTextField* emptyMessage_ = nullptr;
    NSStackView* itemsPanel_ = nullptr;
    std::vector<Entry> entries_;
};

MacOSClippPage::MacOSClippPage(std::function<void()> keyViewChangedHandler)
    : keyViewChangedHandler_(std::move(keyViewChangedHandler))
    , pageState_(std::make_shared<MacOSClippPageState>()) {
    auto state = pageState_;
    keyDerivationWorker_ = std::make_unique<MacOSKeyDerivationWorker>([this, state](const KeyManager::NetworkKey& key) {
        auto keyCopy = std::make_shared<KeyManager::NetworkKey>(key);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (state->alive.load()) {
                this->OnDerivedKey(*keyCopy);
            }
            sodium_memzero(keyCopy->data(), keyCopy->size());
        });
    });

    BuildView();
    SetupPasswordFields();
}

MacOSClippPage::~MacOSClippPage() {
    OnDestroy();
}

NSView* MacOSClippPage::View() const {
    return root_;
}

void MacOSClippPage::OnShown() {
    if (destroyed_) {
        return;
    }

    SetFieldText(networkNameField_, ToNSString(g_settings.networkName()));
    SetupPasswordFields();
    BeginPeerNotifications();
    StartNetworkPollTimer();
    PollNetworkView();

    auto state = pageState_;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (state->alive.load()) {
            this->ScrollToTop();
        }
    });
}

void MacOSClippPage::OnHidden() {
    StopPasswordDebounceTimer();
    StopNetworkPollTimer();
    EndPeerNotifications();
    SetupPasswordFields();
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
    keyDerivationWorker_.reset();
    networkView_.reset();
}

NSView* MacOSClippPage::FirstKeyView() const {
    return networkNameField_;
}

void MacOSClippPage::ConnectKeyViewLoop(NSView* nextKeyView) {
    nextKeyViewAfterPage_ = nextKeyView;

    NSMutableArray<NSView*>* keyViews = [NSMutableArray array];
    if (networkNameField_ != nil) {
        [keyViews addObject:networkNameField_];
    }
    if (passwordField_ != nil) {
        [keyViews addObject:passwordField_];
    }
    if (networkView_) {
        networkView_->AppendKeyViews(keyViews);
    }

    const NSUInteger count = keyViews.count;
    if (count == 0) {
        return;
    }

    for (NSUInteger index = 0; index + 1 < count; ++index) {
        keyViews[index].nextKeyView = keyViews[index + 1];
    }
    keyViews[count - 1].nextKeyView = nextKeyView;
}

void MacOSClippPage::OnFieldEditingBegan(NSTextField* field) {
    if (field == passwordField_ && IsMaskedPasswordPlaceholder(passwordField_)) {
        suppressPasswordChange_ = true;
        SetFieldText(passwordField_, @"");
        suppressPasswordChange_ = false;
    }
}

void MacOSClippPage::OnFieldEditingChanged(NSTextField* field) {
    if (field != passwordField_ || suppressPasswordChange_) {
        return;
    }

    const std::string password = ToStdString(passwordField_.stringValue);
    if (password == kMaskedPassword) {
        return;
    }

    if (!password.empty()) {
        StartPasswordDebounceTimer();
    } else {
        StopPasswordDebounceTimer();
    }
}

void MacOSClippPage::OnFieldEditingEnded(NSTextField* field) {
    if (field == networkNameField_) {
        ApplyNetworkNameChange();
    } else if (field == passwordField_ && ToStdString(passwordField_.stringValue).empty()) {
        StopPasswordDebounceTimer();
        SetupPasswordFields();
    }
}

void MacOSClippPage::SchedulePeerDisplayUpdate() {
    auto state = pageState_;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (state->alive.load() && this->peerDisplayWatcherID_ != 0) {
            this->PollNetworkView();
        }
    });
}

void MacOSClippPage::BuildView() {
    root_ = [[NSView alloc] initWithFrame:NSZeroRect];
    root_.translatesAutoresizingMaskIntoConstraints = NO;

    scrollView_ = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scrollView_.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView_.hasVerticalScroller = YES;
    scrollView_.hasHorizontalScroller = NO;
    scrollView_.autohidesScrollers = YES;
    scrollView_.borderType = NSNoBorder;
    scrollView_.drawsBackground = NO;

    NSView* documentView = [[MacOSFlippedView alloc] initWithFrame:NSZeroRect];
    documentView.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView_.documentView = documentView;

    NSStackView* contentStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    contentStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    contentStack.alignment = NSLayoutAttributeLeading;
    contentStack.distribution = NSStackViewDistributionFill;
    contentStack.spacing = 16.0;
    contentStack.detachesHiddenViews = YES;

    NSTextField* heading = [NSTextField labelWithString:@"Clipp"];
    heading.translatesAutoresizingMaskIntoConstraints = NO;
    heading.font = [NSFont systemFontOfSize:28 weight:NSFontWeightSemibold];
    heading.textColor = [NSColor labelColor];

    NSTextField* intro = MakeWrappingLabel(@"Secure cross-platform clipboard sync with peer-to-peer networking.",
                                           14.0,
                                           [NSColor secondaryLabelColor]);

    NSTextField* networkHeader = [NSTextField labelWithString:@"Network"];
    networkHeader.translatesAutoresizingMaskIntoConstraints = NO;
    networkHeader.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    networkHeader.textColor = [NSColor labelColor];

    NSBox* section = MakeGroupBox();
    networkNameField_ = MakeTextField(240.0);
    networkNameField_.contentType = NSTextContentTypeUsername;
    passwordField_ = MakeSecureTextField(240.0);
    fieldDelegate_ = [[MacOSClippPageFieldDelegate alloc] initWithOwner:this];
    networkNameField_.delegate = fieldDelegate_;
    passwordField_.delegate = fieldDelegate_;

    NSMutableArray<NSLayoutConstraint*>* sectionConstraints = [NSMutableArray array];
    AddInputRow(section, MakeLabel(@"Name"), networkNameField_, nil, sectionConstraints);
    AddInputRow(section, MakeLabel(@"Secret"), passwordField_, networkNameField_, sectionConstraints);
    [sectionConstraints addObject:[passwordField_.bottomAnchor constraintEqualToAnchor:section.bottomAnchor constant:-kSectionInsetY]];
    [NSLayoutConstraint activateConstraints:sectionConstraints];

    passwordStatusPanel_ = MakeGroupBox();
    NSImageView* keyIcon = MakeSymbolImageView(@"key.fill", @"Network key", [NSColor secondaryLabelColor]);
    NSStackView* hashStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    hashStack.translatesAutoresizingMaskIntoConstraints = NO;
    hashStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    hashStack.alignment = NSLayoutAttributeLeading;
    hashStack.spacing = 3.0;

    passwordHashText_ = MakeWrappingLabel(@"", 13.0, [NSColor labelColor]);
    passwordHashText_.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];

    NSTextField* hashExplainer = MakeWrappingLabel(@"Network key fingerprint. Used only on this screen; not in itself a secret.",
                                                   12.0,
                                                   [NSColor secondaryLabelColor]);
    [hashStack addArrangedSubview:passwordHashText_];
    [hashStack addArrangedSubview:hashExplainer];

    [passwordStatusPanel_ addSubview:keyIcon];
    [passwordStatusPanel_ addSubview:hashStack];

    [NSLayoutConstraint activateConstraints:@[
        [keyIcon.leadingAnchor constraintEqualToAnchor:passwordStatusPanel_.leadingAnchor constant:kSectionInsetX],
        [keyIcon.topAnchor constraintEqualToAnchor:passwordStatusPanel_.topAnchor constant:kSectionInsetY],

        [hashStack.leadingAnchor constraintEqualToAnchor:keyIcon.trailingAnchor constant:12.0],
        [hashStack.trailingAnchor constraintEqualToAnchor:passwordStatusPanel_.trailingAnchor constant:-kSectionInsetX],
        [hashStack.topAnchor constraintEqualToAnchor:passwordStatusPanel_.topAnchor constant:kSectionInsetY],
        [hashStack.bottomAnchor constraintEqualToAnchor:passwordStatusPanel_.bottomAnchor constant:-kSectionInsetY],
    ]];

    passwordInfoPanel_ = MakeGroupBox();
    NSImageView* infoIcon = MakeSymbolImageView(@"info.circle.fill", @"Info", [NSColor secondaryLabelColor]);
    passwordInfoText_ = MakeWrappingLabel(@"", 13.0, [NSColor secondaryLabelColor]);

    [passwordInfoPanel_ addSubview:infoIcon];
    [passwordInfoPanel_ addSubview:passwordInfoText_];

    [NSLayoutConstraint activateConstraints:@[
        [infoIcon.leadingAnchor constraintEqualToAnchor:passwordInfoPanel_.leadingAnchor constant:kSectionInsetX],
        [infoIcon.topAnchor constraintEqualToAnchor:passwordInfoPanel_.topAnchor constant:kSectionInsetY],

        [passwordInfoText_.leadingAnchor constraintEqualToAnchor:infoIcon.trailingAnchor constant:12.0],
        [passwordInfoText_.trailingAnchor constraintEqualToAnchor:passwordInfoPanel_.trailingAnchor constant:-kSectionInsetX],
        [passwordInfoText_.topAnchor constraintEqualToAnchor:passwordInfoPanel_.topAnchor constant:kSectionInsetY],
        [passwordInfoText_.bottomAnchor constraintEqualToAnchor:passwordInfoPanel_.bottomAnchor constant:-kSectionInsetY],
    ]];

    networkView_ = std::make_unique<MacOSNetworkView>(g_peerDisplay, [this]() {
        NotifyKeyViewChanged();
    });
    NSView* networkView = networkView_->View();

    [contentStack addArrangedSubview:heading];
    [contentStack addArrangedSubview:intro];
    [contentStack addArrangedSubview:networkHeader];
    [contentStack addArrangedSubview:section];
    [contentStack addArrangedSubview:passwordStatusPanel_];
    [contentStack addArrangedSubview:passwordInfoPanel_];
    [contentStack addArrangedSubview:networkView];

    [documentView addSubview:contentStack];
    [root_ addSubview:scrollView_];

    [NSLayoutConstraint activateConstraints:@[
        [scrollView_.leadingAnchor constraintEqualToAnchor:root_.leadingAnchor],
        [scrollView_.trailingAnchor constraintEqualToAnchor:root_.trailingAnchor],
        [scrollView_.topAnchor constraintEqualToAnchor:root_.topAnchor],
        [scrollView_.bottomAnchor constraintEqualToAnchor:root_.bottomAnchor],

        [documentView.widthAnchor constraintEqualToAnchor:scrollView_.contentView.widthAnchor],

        [contentStack.leadingAnchor constraintEqualToAnchor:documentView.leadingAnchor constant:kPageInset],
        [contentStack.trailingAnchor constraintEqualToAnchor:documentView.trailingAnchor constant:-kPageInset],
        [contentStack.topAnchor constraintEqualToAnchor:documentView.topAnchor constant:kPageInset],
        [contentStack.bottomAnchor constraintEqualToAnchor:documentView.bottomAnchor constant:-kPageInset],

        [heading.widthAnchor constraintLessThanOrEqualToAnchor:contentStack.widthAnchor],
        [intro.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
        [networkHeader.widthAnchor constraintLessThanOrEqualToAnchor:contentStack.widthAnchor],
        [section.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
        [passwordStatusPanel_.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
        [passwordInfoPanel_.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
        [networkView.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
    ]];

    SetFieldText(networkNameField_, ToNSString(g_settings.networkName()));
}

void MacOSClippPage::SetupPasswordFields() {
    if (passwordField_ == nil || passwordHashText_ == nil || passwordStatusPanel_ == nil || passwordInfoPanel_ == nil || passwordInfoText_ == nil) {
        return;
    }

    suppressPasswordChange_ = true;
    if (g_keyManager.HaveNetworkKey()) {
        SetFieldText(passwordField_, @(kMaskedPassword));
        SetFieldText(passwordHashText_, ToNSString(g_keyManager.GetNetworkFingerprintHash()));
        passwordStatusPanel_.hidden = NO;
        passwordInfoPanel_.hidden = YES;
    } else {
        SetFieldText(passwordField_, @"");
        SetFieldText(passwordInfoText_, @"Enter network secret to create or join a network.");
        passwordStatusPanel_.hidden = YES;
        passwordInfoPanel_.hidden = NO;
    }
    suppressPasswordChange_ = false;
}

void MacOSClippPage::NewPasswordHashReceived() {
    if (!g_keyManager.HaveNetworkKey()) {
        return;
    }

    SetFieldText(passwordHashText_, ToNSString(g_keyManager.GetNetworkFingerprintHash()));
    passwordStatusPanel_.hidden = NO;
    passwordInfoPanel_.hidden = YES;
}

void MacOSClippPage::ApplyNetworkNameChange() {
    const std::string newName = ToStdString(networkNameField_.stringValue);
    const std::string currentName = g_settings.networkName();

    if (newName.empty()) {
        SetFieldText(networkNameField_, ToNSString(currentName));
        return;
    }

    if (newName != currentName && g_settings.set_networkName(newName)) {
        g_keyManager.ClearNetworkKey();
        MDNSNotifyNetworkKeyChange();
        g_peerManager.ClearPeers();
        SetupPasswordFields();
    }
}

void MacOSClippPage::StartPasswordDebounceTimer() {
    StopPasswordDebounceTimer();

    auto state = pageState_;
    const uint64_t generation = ++passwordDebounceGeneration_;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(),
                   ^{
        if (state->alive.load() && generation == this->passwordDebounceGeneration_) {
            this->DerivePasswordFromCurrentField();
        }
    });
}

void MacOSClippPage::StopPasswordDebounceTimer() {
    ++passwordDebounceGeneration_;
}

void MacOSClippPage::DerivePasswordFromCurrentField() {
    if (passwordField_ == nil || IsMaskedPasswordPlaceholder(passwordField_)) {
        return;
    }

    const NSString* passwordText = passwordField_.stringValue;
    if (passwordText.length == 0) {
        SetupPasswordFields();
        return;
    }

    passwordStatusPanel_.hidden = YES;
    passwordInfoPanel_.hidden = NO;

    if (passwordText.length < 8) {
        SetFieldText(passwordInfoText_, @"Password must be at least 8 characters.");
        return;
    }

    SetFieldText(passwordInfoText_, @"... working ...");

    const std::string networkName = g_settings.networkName();
    std::string password = ToStdString(passwordField_.stringValue);
    std::string keyInput = networkName + "|";
    keyInput += password;
    keyDerivationWorker_->RequestKeyDerivation(keyInput);
    sodium_memzero(password.data(), password.capacity());
    sodium_memzero(keyInput.data(), keyInput.capacity());
}

void MacOSClippPage::OnDerivedKey(const KeyManager::NetworkKey& key) {
    g_settings.set_networkName(g_settings.networkName());

    std::string errorMessage;
    if (!g_keyManager.SetNetworkKey(key, &errorMessage)) {
        passwordStatusPanel_.hidden = YES;
        passwordInfoPanel_.hidden = NO;
        SetFieldText(passwordInfoText_, ToNSString("Unable to store network key: " + errorMessage));
        return;
    }

    MDNSNotifyNetworkKeyChange();
    g_peerManager.ClearPeers();
    NewPasswordHashReceived();
}

void MacOSClippPage::PollNetworkView() {
    if (networkView_) {
        networkView_->Poll();
    }
}

void MacOSClippPage::ScrollToTop() {
    if (scrollView_ == nil) {
        return;
    }

    [scrollView_ layoutSubtreeIfNeeded];
    [scrollView_.documentView layoutSubtreeIfNeeded];
    [scrollView_.contentView scrollToPoint:NSZeroPoint];
    [scrollView_ reflectScrolledClipView:scrollView_.contentView];
}

void MacOSClippPage::StartNetworkPollTimer() {
    if (networkPollTimer_ != nil) {
        return;
    }

    auto state = pageState_;
    networkPollTimer_ = [NSTimer timerWithTimeInterval:1.0
                                               repeats:YES
                                                 block:^(NSTimer*) {
        if (state->alive.load() && this->peerDisplayWatcherID_ != 0) {
            this->PollNetworkView();
        }
    }];
    [[NSRunLoop mainRunLoop] addTimer:networkPollTimer_ forMode:NSRunLoopCommonModes];
}

void MacOSClippPage::StopNetworkPollTimer() {
    if (networkPollTimer_ != nil) {
        [networkPollTimer_ invalidate];
        networkPollTimer_ = nil;
    }
}

void MacOSClippPage::BeginPeerNotifications() {
    if (peerDisplayWatcherID_ != 0 || destroyed_) {
        return;
    }

    const auto registration = g_peerDisplay.QueryAndRegister(PeerDisplayWatcher, this);
    peerDisplayWatcherID_ = registration.watcherID;
    PollNetworkView();
}

void MacOSClippPage::EndPeerNotifications() {
    if (peerDisplayWatcherID_ == 0) {
        return;
    }

    g_peerDisplay.Unregister(peerDisplayWatcherID_);
    peerDisplayWatcherID_ = 0;
}

void MacOSClippPage::NotifyKeyViewChanged() {
    if (nextKeyViewAfterPage_ != nil) {
        ConnectKeyViewLoop(nextKeyViewAfterPage_);
    }

    if (keyViewChangedHandler_) {
        keyViewChangedHandler_();
    }
}

void MacOSClippPage::PeerDisplayWatcher(const PeerDisplayUpdate&, void* userData) {
    auto* page = reinterpret_cast<MacOSClippPage*>(userData);
    if (page != nullptr) {
        page->SchedulePeerDisplayUpdate();
    }
}

#endif
