#include "NetworkPage.h"

#ifdef __APPLE__

#include "KeyManager.h"
#include "MDNSDiscovery.h"
#include "NetworkRuntime.h"
#include "PeerManager.h"
#include "Settings.h"
#include "platform/uiClippPage.h"
#include "NetworkView.h"
#include "UiHelpers.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include <sodium.h>

#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

extern Settings g_settings;
extern KeyManager g_keyManager;
extern PeerDisplay g_peerDisplay;
extern NetworkRuntime g_networkRuntime;
extern PeerManager g_peerManager;

@interface MacOSNetworkPageFieldDelegate : NSObject <NSTextFieldDelegate> {
@private
    MacOSNetworkPage* owner_;
}
- (instancetype)initWithOwner:(MacOSNetworkPage*)owner;
@end

@implementation MacOSNetworkPageFieldDelegate

- (instancetype)initWithOwner:(MacOSNetworkPage*)owner {
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

namespace {
constexpr CGFloat kPageInset = 28.0;
constexpr CGFloat kSectionInsetX = 18.0;
constexpr CGFloat kSectionInsetY = 14.0;
constexpr char kMaskedPassword[] = "****************";

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

bool IsMaskedPasswordPlaceholder(NSTextField* field) {
    return field != nil && MacOSToStdString(field.stringValue) == kMaskedPassword;
}
}

struct MacOSNetworkPageState {
    std::atomic_bool alive{ true };
};

MacOSNetworkPage::MacOSNetworkPage(std::function<void()> keyViewChangedHandler, std::function<void()> networkKeyChangedHandler)
    : keyViewChangedHandler_(std::move(keyViewChangedHandler))
    , networkKeyChangedHandler_(std::move(networkKeyChangedHandler))
    , pageState_(std::make_shared<MacOSNetworkPageState>()) {
    auto state = pageState_;
    keyDerivationWorker_ = std::make_unique<uiClippPage::KeyDerivationWorker>([this, state](const KeyManager::NetworkKey& key) {
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

MacOSNetworkPage::~MacOSNetworkPage() {
    OnDestroy();
}

NSView* MacOSNetworkPage::View() const {
    return root_;
}

void MacOSNetworkPage::OnShown() {
    if (destroyed_) {
        return;
    }

    MacOSSetFieldText(networkNameField_, MacOSToNSString(g_settings.networkName()));
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

void MacOSNetworkPage::OnHidden() {
    StopPasswordDebounceTimer();
    StopNetworkPollTimer();
    EndPeerNotifications();
    SetupPasswordFields();
}

void MacOSNetworkPage::OnDestroy() {
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

NSView* MacOSNetworkPage::FirstKeyView() const {
    return networkNameField_;
}

void MacOSNetworkPage::ConnectKeyViewLoop(NSView* nextKeyView) {
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

void MacOSNetworkPage::OnFieldEditingBegan(NSTextField* field) {
    if (field == passwordField_ && IsMaskedPasswordPlaceholder(passwordField_)) {
        suppressPasswordChange_ = true;
        MacOSSetFieldText(passwordField_, @"");
        suppressPasswordChange_ = false;
    }
}

void MacOSNetworkPage::OnFieldEditingChanged(NSTextField* field) {
    if (field != passwordField_ || suppressPasswordChange_) {
        return;
    }

    const std::string password = MacOSToStdString(passwordField_.stringValue);
    if (password == kMaskedPassword) {
        return;
    }

    if (!password.empty()) {
        StartPasswordDebounceTimer();
    } else {
        StopPasswordDebounceTimer();
    }
}

void MacOSNetworkPage::OnFieldEditingEnded(NSTextField* field) {
    if (field == networkNameField_) {
        ApplyNetworkNameChange();
    } else if (field == passwordField_ && MacOSToStdString(passwordField_.stringValue).empty()) {
        StopPasswordDebounceTimer();
        SetupPasswordFields();
    }
}

void MacOSNetworkPage::SchedulePeerDisplayUpdate() {
    auto state = pageState_;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (state->alive.load() && this->peerDisplayWatcherID_ != 0) {
            this->PollNetworkView();
        }
    });
}

void MacOSNetworkPage::BuildView() {
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

    NSTextField* heading = [NSTextField labelWithString:CLP_NS(CLP_UI_SYNC_GROUP)];
    heading.translatesAutoresizingMaskIntoConstraints = NO;
    heading.font = [NSFont systemFontOfSize:28 weight:NSFontWeightSemibold];
    heading.textColor = [NSColor labelColor];

    NSTextField* networkHeader = [NSTextField labelWithString:CLP_NS(CLP_UI_NETWORK_KEY)];
    networkHeader.translatesAutoresizingMaskIntoConstraints = NO;
    networkHeader.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    networkHeader.textColor = [NSColor labelColor];

    NSBox* section = MacOSMakeGroupBox();
    networkNameField_ = MacOSMakeTextField(240.0);
    networkNameField_.contentType = NSTextContentTypeUsername;
    passwordField_ = MacOSMakeSecureTextField(240.0);
    fieldDelegate_ = [[MacOSNetworkPageFieldDelegate alloc] initWithOwner:this];
    networkNameField_.delegate = fieldDelegate_;
    passwordField_.delegate = fieldDelegate_;

    NSMutableArray<NSLayoutConstraint*>* sectionConstraints = [NSMutableArray array];
    AddInputRow(section, MacOSMakeLabel(CLP_NS(CLP_UI_NAME)), networkNameField_, nil, sectionConstraints);
    AddInputRow(section, MacOSMakeLabel(CLP_NS(CLP_UI_SECRET)), passwordField_, networkNameField_, sectionConstraints);
    [sectionConstraints addObject:[passwordField_.bottomAnchor constraintEqualToAnchor:section.bottomAnchor constant:-kSectionInsetY]];
    [NSLayoutConstraint activateConstraints:sectionConstraints];

    passwordStatusPanel_ = MacOSMakeGroupBox();
    NSImageView* keyIcon = MacOSMakeSymbolImageView(@"key.fill", CLP_NS(CLP_UI_NETWORK_KEY), [NSColor secondaryLabelColor]);
    NSStackView* hashStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    hashStack.translatesAutoresizingMaskIntoConstraints = NO;
    hashStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    hashStack.alignment = NSLayoutAttributeLeading;
    hashStack.spacing = 3.0;

    passwordHashText_ = MacOSMakeWrappingLabel(@"", 13.0, [NSColor labelColor]);
    passwordHashText_.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];

    NSTextField* hashExplainer = MacOSMakeWrappingLabel(CLP_NS(CLP_UI_NETWORK_KEY_FINGERPRINT),
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

    passwordInfoPanel_ = MacOSMakeGroupBox();
    NSImageView* infoIcon = MacOSMakeSymbolImageView(@"info.circle.fill", @"Info", [NSColor secondaryLabelColor]);
    passwordInfoText_ = MacOSMakeWrappingLabel(@"", 13.0, [NSColor secondaryLabelColor]);

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
        [networkHeader.widthAnchor constraintLessThanOrEqualToAnchor:contentStack.widthAnchor],
        [section.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
        [passwordStatusPanel_.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
        [passwordInfoPanel_.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
        [networkView.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
    ]];

    MacOSSetFieldText(networkNameField_, MacOSToNSString(g_settings.networkName()));
}

void MacOSNetworkPage::SetupPasswordFields() {
    if (passwordField_ == nil || passwordHashText_ == nil || passwordStatusPanel_ == nil || passwordInfoPanel_ == nil || passwordInfoText_ == nil) {
        return;
    }

    suppressPasswordChange_ = true;
    if (g_keyManager.HaveNetworkKey()) {
        MacOSSetFieldText(passwordField_, @(kMaskedPassword));
        MacOSSetFieldText(passwordHashText_, MacOSToNSString(g_keyManager.GetNetworkFingerprintHash()));
        passwordStatusPanel_.hidden = NO;
        passwordInfoPanel_.hidden = YES;
    } else {
        MacOSSetFieldText(passwordField_, @"");
        MacOSSetFieldText(passwordInfoText_, CLP_NS(CLP_UI_ENTER_NETWORK_SECRET));
        passwordStatusPanel_.hidden = YES;
        passwordInfoPanel_.hidden = NO;
    }
    suppressPasswordChange_ = false;
}

void MacOSNetworkPage::NewPasswordHashReceived() {
    if (!g_keyManager.HaveNetworkKey()) {
        return;
    }

    MacOSSetFieldText(passwordHashText_, MacOSToNSString(g_keyManager.GetNetworkFingerprintHash()));
    passwordStatusPanel_.hidden = NO;
    passwordInfoPanel_.hidden = YES;
}

void MacOSNetworkPage::ApplyNetworkNameChange() {
    const std::string newName = MacOSToStdString(networkNameField_.stringValue);
    const std::string currentName = g_settings.networkName();

    if (newName.empty()) {
        MacOSSetFieldText(networkNameField_, MacOSToNSString(currentName));
        return;
    }

    if (newName != currentName && g_settings.set_networkName(newName)) {
        g_keyManager.ClearNetworkKey();
        g_networkRuntime.Restart();
        SetupPasswordFields();
        if (networkKeyChangedHandler_) {
            networkKeyChangedHandler_();
        }
    }
}

void MacOSNetworkPage::StartPasswordDebounceTimer() {
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

void MacOSNetworkPage::StopPasswordDebounceTimer() {
    ++passwordDebounceGeneration_;
}

void MacOSNetworkPage::DerivePasswordFromCurrentField() {
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
        MacOSSetFieldText(passwordInfoText_, CLP_NS(CLP_UI_SECRET_TOO_SHORT));
        return;
    }

    MacOSSetFieldText(passwordInfoText_, CLP_NS(CLP_UI_WORKING));

    const std::string networkName = g_settings.networkName();
    std::string password = MacOSToStdString(passwordField_.stringValue);
    std::string keyInput = KeyManager::BuildKeyDerivationInput(networkName, password);
    keyDerivationWorker_->RequestKeyDerivation(keyInput);
    sodium_memzero(password.data(), password.capacity());
    sodium_memzero(keyInput.data(), keyInput.capacity());
}

void MacOSNetworkPage::OnDerivedKey(const KeyManager::NetworkKey& key) {
    g_settings.set_networkName(g_settings.networkName());

    std::string errorMessage;
    if (!g_keyManager.SetNetworkKey(key, &errorMessage)) {
        passwordStatusPanel_.hidden = YES;
        passwordInfoPanel_.hidden = NO;
        MacOSSetFieldText(passwordInfoText_, MacOSToNSString("Unable to store network key: " + errorMessage));
        return;
    }

    g_networkRuntime.Restart();
    NewPasswordHashReceived();
    if (networkKeyChangedHandler_) {
        networkKeyChangedHandler_();
    }
}

void MacOSNetworkPage::PollNetworkView() {
    if (networkView_) {
        networkView_->Poll();
    }
}

void MacOSNetworkPage::ScrollToTop() {
    if (scrollView_ == nil) {
        return;
    }

    [scrollView_ layoutSubtreeIfNeeded];
    [scrollView_.documentView layoutSubtreeIfNeeded];
    [scrollView_.contentView scrollToPoint:NSZeroPoint];
    [scrollView_ reflectScrolledClipView:scrollView_.contentView];
}

void MacOSNetworkPage::StartNetworkPollTimer() {
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

void MacOSNetworkPage::StopNetworkPollTimer() {
    if (networkPollTimer_ != nil) {
        [networkPollTimer_ invalidate];
        networkPollTimer_ = nil;
    }
}

void MacOSNetworkPage::BeginPeerNotifications() {
    if (peerDisplayWatcherID_ != 0 || destroyed_) {
        return;
    }

    const auto registration = g_peerDisplay.QueryAndRegister(PeerDisplayWatcher, this);
    peerDisplayWatcherID_ = registration.watcherID;
    PollNetworkView();
}

void MacOSNetworkPage::EndPeerNotifications() {
    if (peerDisplayWatcherID_ == 0) {
        return;
    }

    g_peerDisplay.Unregister(peerDisplayWatcherID_);
    peerDisplayWatcherID_ = 0;
}

void MacOSNetworkPage::NotifyKeyViewChanged() {
    if (nextKeyViewAfterPage_ != nil) {
        ConnectKeyViewLoop(nextKeyViewAfterPage_);
    }

    if (keyViewChangedHandler_) {
        keyViewChangedHandler_();
    }
}

void MacOSNetworkPage::PeerDisplayWatcher(const PeerDisplayUpdate&, void* userData) {
    auto* page = reinterpret_cast<MacOSNetworkPage*>(userData);
    if (page != nullptr) {
        page->SchedulePeerDisplayUpdate();
    }
}

#endif
