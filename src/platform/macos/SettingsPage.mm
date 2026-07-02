#include "SettingsPage.h"

#ifdef __APPLE__

#include "ClipboardActivityStore.h"
#include "MDNSDiscovery.h"
#include "NetworkRuntime.h"
#include "PeerManager.h"
#include "Settings.h"
#include "platform.h"
#include "platform/uiSettingsPage.h"
#include "platform/uistrings.h"
#include "UiHelpers.h"

#include <cstddef>
#include <cstdint>

#import <AppKit/AppKit.h>

extern Settings g_settings;
extern NetworkRuntime g_networkRuntime;
extern PeerManager g_peerManager;
extern ClipboardActivityStore g_clipboardActivityStore;

@interface MacOSSettingsPageFieldDelegate : NSObject <NSTextFieldDelegate> {
@private
    MacOSSettingsPage* owner_;
}
- (instancetype)initWithOwner:(MacOSSettingsPage*)owner;
- (void)historySliderChanged:(id)sender;
- (void)resetHostID:(id)sender;
- (void)honorPrivacyMarkersChanged:(id)sender;
- (void)maskShortTextPreviewsChanged:(id)sender;
- (void)launchAtLoginChanged:(id)sender;
@end

@implementation MacOSSettingsPageFieldDelegate

- (instancetype)initWithOwner:(MacOSSettingsPage*)owner {
    self = [super init];
    if (self) {
        owner_ = owner;
    }
    return self;
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    if (owner_ == nullptr || ![notification.object isKindOfClass:[NSTextField class]]) {
        return;
    }

    owner_->OnFieldEditingEnded(static_cast<NSTextField*>(notification.object));
}

- (void)historySliderChanged:(id)sender {
    (void)sender;
    if (owner_ != nullptr) {
        owner_->OnHistorySliderChanged();
    }
}

- (void)resetHostID:(id)sender {
    (void)sender;
    if (owner_ != nullptr) {
        owner_->OnResetHostID();
    }
}

- (void)honorPrivacyMarkersChanged:(id)sender {
    (void)sender;
    if (owner_ != nullptr) {
        owner_->OnHonorPrivacyMarkersChanged();
    }
}

- (void)maskShortTextPreviewsChanged:(id)sender {
    (void)sender;
    if (owner_ != nullptr) {
        owner_->OnMaskShortTextPreviewsChanged();
    }
}

- (void)launchAtLoginChanged:(id)sender {
    (void)sender;
    if (owner_ != nullptr) {
        owner_->OnLaunchAtLoginChanged();
    }
}

@end

@interface MacOSSettingsFlippedView : NSView
@end

@implementation MacOSSettingsFlippedView

- (BOOL)isFlipped {
    return YES;
}

@end

namespace {
    struct LimitStop {
        uint64_t value;
        NSString* label;
    };

    constexpr uint64_t kMiB = 1024ull * 1024ull;
    constexpr uint64_t kGiB = 1024ull * kMiB;

    static const LimitStop kHistoryMemoryStops[] = {
        { 1ull * kMiB, @"1 MB" },
        { 8ull * kMiB, @"8 MB" },
        { 32ull * kMiB, @"32 MB" },
        { 128ull * kMiB, @"128 MB" },
        { Settings::DefaultClipboardHistoryMemoryLimitBytes, @"256 MB" },
        { 512ull * kMiB, @"512 MB" },
        { 1ull * kGiB, @"1 GB" },
        { 2ull * kGiB, @"2 GB" },
        { Settings::UnlimitedClipboardHistoryLimit, CLP_NS(CLP_UI_UNLIMITED) },
    };

    static const LimitStop kHistoryAgeStops[] = {
        { 1, @"1 second" },
        { 10, @"10 seconds" },
        { 60, @"1 minute" },
        { 10ull * 60ull, @"10 minutes" },
        { 60ull * 60ull, @"1 hour" },
        { 6ull * 60ull * 60ull, @"6 hours" },
        { Settings::DefaultClipboardHistoryMaxAgeSeconds, @"1 day" },
        { 7ull * 24ull * 60ull * 60ull, @"7 days" },
        { 30ull * 24ull * 60ull * 60ull, @"30 days" },
        { Settings::UnlimitedClipboardHistoryLimit, CLP_NS(CLP_UI_UNLIMITED) },
    };

    static const LimitStop kHistoryItemStops[] = {
        { 1, @"1 item" },
        { 10, @"10 items" },
        { 50, @"50 items" },
        { 100, @"100 items" },
        { 500, @"500 items" },
        { Settings::DefaultClipboardHistoryMaxItems, @"1000 items" },
        { 5000, @"5000 items" },
        { 10000, @"10000 items" },
        { Settings::UnlimitedClipboardHistoryLimit, CLP_NS(CLP_UI_UNLIMITED) },
    };

    constexpr CGFloat kPageInset = 28.0;
    constexpr CGFloat kSectionInsetX = 18.0;
    constexpr CGFloat kSectionInsetY = 14.0;

    static void AddSettingRow(
        NSView* section,
        NSTextField* label,
        NSTextField* field,
        NSTextField* previousField,
        NSMutableArray<NSLayoutConstraint*>* constraints)
    {
        [section addSubview:label];
        [section addSubview:field];

        [constraints addObjectsFromArray:@[
            [label.leadingAnchor constraintEqualToAnchor:section.leadingAnchor constant:kSectionInsetX],
            [label.widthAnchor constraintEqualToConstant:130.0],
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

    static NSSlider* MakeLimitSlider(NSUInteger stopCount, id target) {
        NSSlider* slider = [NSSlider sliderWithValue:0.0
                                            minValue:0.0
                                            maxValue:stopCount > 0 ? static_cast<double>(stopCount - 1) : 0.0
                                              target:target
                                              action:@selector(historySliderChanged:)];
        slider.translatesAutoresizingMaskIntoConstraints = NO;
        slider.numberOfTickMarks = static_cast<NSInteger>(stopCount);
        slider.allowsTickMarkValuesOnly = YES;
        slider.continuous = YES;
        return slider;
    }

    static NSTextField* MakeLimitValueLabel() {
        NSTextField* label = [NSTextField labelWithString:@""];
        label.translatesAutoresizingMaskIntoConstraints = NO;
        label.font = [NSFont systemFontOfSize:13];
        label.textColor = [NSColor secondaryLabelColor];
        label.alignment = NSTextAlignmentRight;
        [label.widthAnchor constraintEqualToConstant:112.0].active = YES;
        return label;
    }

    static void AddSliderSettingRow(
        NSView* section,
        NSTextField* label,
        NSSlider* slider,
        NSTextField* value,
        NSView* previousField,
        NSMutableArray<NSLayoutConstraint*>* constraints)
    {
        [section addSubview:label];
        [section addSubview:slider];
        [section addSubview:value];

        [constraints addObjectsFromArray:@[
            [label.leadingAnchor constraintEqualToAnchor:section.leadingAnchor constant:kSectionInsetX],
            [label.widthAnchor constraintEqualToConstant:130.0],
            [label.centerYAnchor constraintEqualToAnchor:slider.centerYAnchor],

            [slider.leadingAnchor constraintEqualToAnchor:label.trailingAnchor constant:16.0],
            [slider.trailingAnchor constraintEqualToAnchor:value.leadingAnchor constant:-16.0],

            [value.trailingAnchor constraintEqualToAnchor:section.trailingAnchor constant:-kSectionInsetX],
            [value.centerYAnchor constraintEqualToAnchor:slider.centerYAnchor],
        ]];

        if (previousField == nil) {
            [constraints addObject:[slider.topAnchor constraintEqualToAnchor:section.topAnchor constant:kSectionInsetY]];
        } else {
            [constraints addObject:[slider.topAnchor constraintEqualToAnchor:previousField.bottomAnchor constant:12.0]];
        }
    }

    static NSTextField* MakeHostIDValueLabel() {
        NSTextField* label = [NSTextField labelWithString:@""];
        label.translatesAutoresizingMaskIntoConstraints = NO;
        label.font = [NSFont monospacedSystemFontOfSize:12.0 weight:NSFontWeightRegular];
        label.textColor = [NSColor labelColor];
        label.lineBreakMode = NSLineBreakByTruncatingMiddle;
        label.maximumNumberOfLines = 1;
        return label;
    }

    static NSButton* MakeResetButton(id target) {
        NSButton* button = [NSButton buttonWithTitle:CLP_NS(CLP_UI_RESET)
                                              target:target
                                              action:@selector(resetHostID:)];
        button.translatesAutoresizingMaskIntoConstraints = NO;
        button.bezelStyle = NSBezelStyleRounded;
        return button;
    }

    static void AddHostIDRow(
        NSView* section,
        NSTextField* label,
        NSTextField* value,
        NSButton* resetButton,
        NSMutableArray<NSLayoutConstraint*>* constraints)
    {
        [section addSubview:label];
        [section addSubview:value];
        [section addSubview:resetButton];

        [constraints addObjectsFromArray:@[
            [label.leadingAnchor constraintEqualToAnchor:section.leadingAnchor constant:kSectionInsetX],
            [label.widthAnchor constraintEqualToConstant:130.0],
            [label.centerYAnchor constraintEqualToAnchor:value.centerYAnchor],

            [value.leadingAnchor constraintEqualToAnchor:label.trailingAnchor constant:16.0],
            [value.trailingAnchor constraintLessThanOrEqualToAnchor:resetButton.leadingAnchor constant:-16.0],
            [value.centerYAnchor constraintEqualToAnchor:resetButton.centerYAnchor],
            [value.topAnchor constraintEqualToAnchor:section.topAnchor constant:kSectionInsetY],

            [resetButton.trailingAnchor constraintEqualToAnchor:section.trailingAnchor constant:-kSectionInsetX],
            [resetButton.topAnchor constraintEqualToAnchor:section.topAnchor constant:kSectionInsetY - 3.0],
            [resetButton.bottomAnchor constraintEqualToAnchor:section.bottomAnchor constant:-(kSectionInsetY - 3.0)],
        ]];
    }

    template <std::size_t N>
    std::size_t FindStopIndex(const LimitStop(&stops)[N], uint64_t value) {
        for (std::size_t i = 0; i < N; ++i) {
            if (stops[i].value == value) {
                return i;
            }
        }

        for (std::size_t i = 0; i + 1 < N; ++i) {
            if (value <= stops[i].value) {
                return i;
            }
        }

        return N - 1;
    }

    template <std::size_t N>
    std::size_t SliderStopIndex(NSSlider* slider, const LimitStop(&)[N]) {
        if (slider == nil || slider.doubleValue <= 0.0) {
            return 0;
        }

        const auto index = static_cast<std::size_t>(slider.doubleValue + 0.5);
        return index < N ? index : N - 1;
    }

    template <std::size_t N>
    uint64_t SliderStopValue(NSSlider* slider, const LimitStop(&stops)[N]) {
        return stops[SliderStopIndex(slider, stops)].value;
    }

    template <std::size_t N>
    NSString* SliderStopLabel(NSSlider* slider, const LimitStop(&stops)[N]) {
        return stops[SliderStopIndex(slider, stops)].label;
    }
}

MacOSSettingsPage::MacOSSettingsPage() {
    BuildView();
    LoadSettingsIntoFields();
}

NSView* MacOSSettingsPage::View() const {
    return root_;
}

void MacOSSettingsPage::OnShown() {
    LoadSettingsIntoFields();
}

NSView* MacOSSettingsPage::FirstKeyView() const {
    return historyMemorySlider_ != nil ? historyMemorySlider_ : tcpPortField_;
}

void MacOSSettingsPage::ConnectKeyViewLoop(NSView* nextKeyView) {
    if (historyMemorySlider_ == nil || historyAgeSlider_ == nil || historyItemSlider_ == nil ||
        tcpPortField_ == nil || listenerIpField_ == nil) {
        return;
    }

    historyMemorySlider_.nextKeyView = historyAgeSlider_;
    historyAgeSlider_.nextKeyView = historyItemSlider_;
    historyItemSlider_.nextKeyView = tcpPortField_;
    tcpPortField_.nextKeyView = listenerIpField_;
    listenerIpField_.nextKeyView = nextKeyView;
}

void MacOSSettingsPage::OnFieldEditingEnded(NSTextField* field) {
    if (field == tcpPortField_) {
        ValidateTcpPort();
    } else if (field == listenerIpField_) {
        ValidateListenerIp();
    }
}

void MacOSSettingsPage::OnHistorySliderChanged() {
    ApplyClipboardHistorySettingChange();
}

void MacOSSettingsPage::OnResetHostID() {
    ResetHostID();
}

void MacOSSettingsPage::OnHonorPrivacyMarkersChanged() {
    ApplyPrivacySettingChange();
}

void MacOSSettingsPage::OnMaskShortTextPreviewsChanged() {
    ApplyPrivacySettingChange();
}

void MacOSSettingsPage::OnLaunchAtLoginChanged() {
    ApplyLaunchAtLoginChange();
}

void MacOSSettingsPage::BuildView() {
    NSScrollView* scrollView = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView.hasVerticalScroller = YES;
    scrollView.hasHorizontalScroller = NO;
    scrollView.autohidesScrollers = YES;
    scrollView.borderType = NSNoBorder;
    scrollView.drawsBackground = NO;
    root_ = scrollView;

    NSView* contentRoot = [[MacOSSettingsFlippedView alloc] initWithFrame:NSZeroRect];
    contentRoot.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView.documentView = contentRoot;

    NSTextField* heading = [NSTextField labelWithString:CLP_NS(CLP_UI_SETTINGS)];
    heading.translatesAutoresizingMaskIntoConstraints = NO;
    heading.font = [NSFont systemFontOfSize:28 weight:NSFontWeightSemibold];
    heading.textColor = [NSColor labelColor];

    fieldDelegate_ = [[MacOSSettingsPageFieldDelegate alloc] initWithOwner:this];

    // MAS guideline 2.4.5(iii): the App Store flavor must not register a login
    // item without explicit consent, so it gets an opt-in toggle here. Every
    // other flavor autostarts by default and omits the section entirely.
    NSTextField* startupHeader = nil;
    NSBox* startupSection = nil;
    NSMutableArray<NSLayoutConstraint*>* startupConstraints = [NSMutableArray array];
    if (IsMacAppStoreBuild()) {
        startupHeader = [NSTextField labelWithString:CLP_NS(CLP_UI_STARTUP)];
        startupHeader.translatesAutoresizingMaskIntoConstraints = NO;
        startupHeader.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
        startupHeader.textColor = [NSColor labelColor];

        startupSection = MacOSMakeGroupBox();
        launchAtLoginCheckbox_ = [NSButton checkboxWithTitle:CLP_NS(CLP_UI_LAUNCH_AT_LOGIN)
                                                      target:fieldDelegate_
                                                      action:@selector(launchAtLoginChanged:)];
        launchAtLoginCheckbox_.translatesAutoresizingMaskIntoConstraints = NO;
        [startupSection addSubview:launchAtLoginCheckbox_];

        [startupConstraints addObjectsFromArray:@[
            [launchAtLoginCheckbox_.leadingAnchor constraintEqualToAnchor:startupSection.leadingAnchor constant:kSectionInsetX],
            [launchAtLoginCheckbox_.trailingAnchor constraintLessThanOrEqualToAnchor:startupSection.trailingAnchor constant:-kSectionInsetX],
            [launchAtLoginCheckbox_.topAnchor constraintEqualToAnchor:startupSection.topAnchor constant:kSectionInsetY],
            [launchAtLoginCheckbox_.bottomAnchor constraintEqualToAnchor:startupSection.bottomAnchor constant:-kSectionInsetY],
        ]];
    }

    NSTextField* historyHeader = [NSTextField labelWithString:CLP_NS(CLP_UI_CLIPBOARD_HISTORY)];
    historyHeader.translatesAutoresizingMaskIntoConstraints = NO;
    historyHeader.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    historyHeader.textColor = [NSColor labelColor];

    NSBox* historySection = MacOSMakeGroupBox();
    historyMemorySlider_ = MakeLimitSlider(cntof(kHistoryMemoryStops), fieldDelegate_);
    historyAgeSlider_ = MakeLimitSlider(cntof(kHistoryAgeStops), fieldDelegate_);
    historyItemSlider_ = MakeLimitSlider(cntof(kHistoryItemStops), fieldDelegate_);
    historyMemoryValue_ = MakeLimitValueLabel();
    historyAgeValue_ = MakeLimitValueLabel();
    historyItemValue_ = MakeLimitValueLabel();

    NSMutableArray<NSLayoutConstraint*>* historyConstraints = [NSMutableArray array];
    AddSliderSettingRow(historySection, MacOSMakeLabel(CLP_NS(CLP_UI_HISTORY_MEMORY_LIMIT)), historyMemorySlider_, historyMemoryValue_, nil, historyConstraints);
    AddSliderSettingRow(historySection, MacOSMakeLabel(CLP_NS(CLP_UI_HISTORY_TIME_LIMIT)), historyAgeSlider_, historyAgeValue_, historyMemorySlider_, historyConstraints);
    AddSliderSettingRow(historySection, MacOSMakeLabel(CLP_NS(CLP_UI_HISTORY_ITEM_LIMIT)), historyItemSlider_, historyItemValue_, historyAgeSlider_, historyConstraints);
    [historyConstraints addObject:[historyItemSlider_.bottomAnchor constraintEqualToAnchor:historySection.bottomAnchor constant:-kSectionInsetY]];

    NSTextField* networkHeader = [NSTextField labelWithString:CLP_NS(CLP_UI_NETWORK)];
    networkHeader.translatesAutoresizingMaskIntoConstraints = NO;
    networkHeader.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    networkHeader.textColor = [NSColor labelColor];

    NSBox* section = MacOSMakeGroupBox();

    tcpPortField_ = MacOSMakeFixedWidthTextField(110.0);
    listenerIpField_ = MacOSMakeFixedWidthTextField(190.0);
    tcpPortField_.delegate = fieldDelegate_;
    listenerIpField_.delegate = fieldDelegate_;

    NSMutableArray<NSLayoutConstraint*>* rowConstraints = [NSMutableArray array];
    AddSettingRow(section, MacOSMakeLabel(CLP_NS(CLP_UI_TCP_PORT)), tcpPortField_, nil, rowConstraints);
    AddSettingRow(section, MacOSMakeLabel(CLP_NS(CLP_UI_LISTENER_IP)), listenerIpField_, tcpPortField_, rowConstraints);
    [rowConstraints addObject:[listenerIpField_.bottomAnchor constraintEqualToAnchor:section.bottomAnchor constant:-kSectionInsetY]];

    NSTextField* privacyHeader = [NSTextField labelWithString:CLP_NS(CLP_UI_PRIVACY)];
    privacyHeader.translatesAutoresizingMaskIntoConstraints = NO;
    privacyHeader.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    privacyHeader.textColor = [NSColor labelColor];

    NSBox* privacySection = MacOSMakeGroupBox();

    maskShortTextPreviewsCheckbox_ = [NSButton checkboxWithTitle:CLP_NS(CLP_UI_MASK_SHORT_TEXT_PREVIEWS)
                                                          target:fieldDelegate_
                                                          action:@selector(maskShortTextPreviewsChanged:)];
    maskShortTextPreviewsCheckbox_.translatesAutoresizingMaskIntoConstraints = NO;
    [privacySection addSubview:maskShortTextPreviewsCheckbox_];

    NSTextField* maskHelp = MacOSMakeWrappingLabel(CLP_NS(CLP_UI_MASK_SHORT_TEXT_PREVIEWS_HELP),
                                                   12.0,
                                                   [NSColor secondaryLabelColor]);
    [privacySection addSubview:maskHelp];

    honorPrivacyMarkersCheckbox_ = [NSButton checkboxWithTitle:CLP_NS(CLP_UI_HONOR_PRIVACY_MARKERS)
                                                        target:fieldDelegate_
                                                        action:@selector(honorPrivacyMarkersChanged:)];
    honorPrivacyMarkersCheckbox_.translatesAutoresizingMaskIntoConstraints = NO;
    [privacySection addSubview:honorPrivacyMarkersCheckbox_];

    NSTextField* privacyHelp = MacOSMakeWrappingLabel(CLP_NS(CLP_UI_HONOR_PRIVACY_MARKERS_HELP),
                                                      12.0,
                                                      [NSColor secondaryLabelColor]);
    [privacySection addSubview:privacyHelp];

    NSMutableArray<NSLayoutConstraint*>* privacyConstraints = [NSMutableArray array];
    [privacyConstraints addObjectsFromArray:@[
        [maskShortTextPreviewsCheckbox_.leadingAnchor constraintEqualToAnchor:privacySection.leadingAnchor constant:kSectionInsetX],
        [maskShortTextPreviewsCheckbox_.trailingAnchor constraintLessThanOrEqualToAnchor:privacySection.trailingAnchor constant:-kSectionInsetX],
        [maskShortTextPreviewsCheckbox_.topAnchor constraintEqualToAnchor:privacySection.topAnchor constant:kSectionInsetY],

        [maskHelp.leadingAnchor constraintEqualToAnchor:privacySection.leadingAnchor constant:kSectionInsetX],
        [maskHelp.trailingAnchor constraintEqualToAnchor:privacySection.trailingAnchor constant:-kSectionInsetX],
        [maskHelp.topAnchor constraintEqualToAnchor:maskShortTextPreviewsCheckbox_.bottomAnchor constant:6.0],

        [honorPrivacyMarkersCheckbox_.leadingAnchor constraintEqualToAnchor:privacySection.leadingAnchor constant:kSectionInsetX],
        [honorPrivacyMarkersCheckbox_.trailingAnchor constraintLessThanOrEqualToAnchor:privacySection.trailingAnchor constant:-kSectionInsetX],
        [honorPrivacyMarkersCheckbox_.topAnchor constraintEqualToAnchor:maskHelp.bottomAnchor constant:14.0],

        [privacyHelp.leadingAnchor constraintEqualToAnchor:privacySection.leadingAnchor constant:kSectionInsetX],
        [privacyHelp.trailingAnchor constraintEqualToAnchor:privacySection.trailingAnchor constant:-kSectionInsetX],
        [privacyHelp.topAnchor constraintEqualToAnchor:honorPrivacyMarkersCheckbox_.bottomAnchor constant:6.0],
        [privacyHelp.bottomAnchor constraintEqualToAnchor:privacySection.bottomAnchor constant:-kSectionInsetY],
    ]];

    NSTextField* hostIDHeader = [NSTextField labelWithString:CLP_NS(CLP_UI_HOST_ID)];
    hostIDHeader.translatesAutoresizingMaskIntoConstraints = NO;
    hostIDHeader.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    hostIDHeader.textColor = [NSColor labelColor];

    NSBox* hostIDSection = MacOSMakeGroupBox();
    hostIDValue_ = MakeHostIDValueLabel();
    resetHostIDButton_ = MakeResetButton(fieldDelegate_);

    NSMutableArray<NSLayoutConstraint*>* hostIDConstraints = [NSMutableArray array];
    AddHostIDRow(hostIDSection, MacOSMakeLabel(CLP_NS(CLP_UI_CURRENT_HOST_ID)), hostIDValue_, resetHostIDButton_, hostIDConstraints);

    hostIDWarningContainer_ = [[NSView alloc] initWithFrame:NSZeroRect];
    hostIDWarningContainer_.translatesAutoresizingMaskIntoConstraints = NO;
    hostIDWarningContainer_.hidden = YES;
    hostIDWarningText_ = MacOSMakeWrappingLabel(CLP_NS(CLP_UI_HOST_ID_COLLISION_WARNING),
                                                13.0,
                                                [NSColor systemOrangeColor]);
    [hostIDWarningContainer_ addSubview:hostIDWarningText_];

    [contentRoot addSubview:heading];
    if (startupSection != nil) {
        [contentRoot addSubview:startupHeader];
        [contentRoot addSubview:startupSection];
    }
    [contentRoot addSubview:historyHeader];
    [contentRoot addSubview:historySection];
    [contentRoot addSubview:privacyHeader];
    [contentRoot addSubview:privacySection];
    [contentRoot addSubview:networkHeader];
    [contentRoot addSubview:section];
    [contentRoot addSubview:hostIDHeader];
    [contentRoot addSubview:hostIDSection];
    [contentRoot addSubview:hostIDWarningContainer_];

    statusContainer_ = [[NSView alloc] initWithFrame:NSZeroRect];
    statusContainer_.translatesAutoresizingMaskIntoConstraints = NO;
    statusContainer_.hidden = YES;

    statusMessage_ = MacOSMakeWrappingLabel(CLP_NS(CLP_UI_NETWORK_SETTINGS_APPLIED), 13.0, [NSColor secondaryLabelColor]);
    [statusContainer_ addSubview:statusMessage_];
    [contentRoot addSubview:statusContainer_];

    NSView* historyTopAnchorView = heading;
    CGFloat historyTopSpacing = 16.0;
    if (startupSection != nil) {
        [startupConstraints addObjectsFromArray:@[
            [startupHeader.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
            [startupHeader.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
            [startupHeader.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:16.0],

            [startupSection.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
            [startupSection.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
            [startupSection.topAnchor constraintEqualToAnchor:startupHeader.bottomAnchor constant:16.0],
        ]];
        historyTopAnchorView = startupSection;
        historyTopSpacing = 18.0;
    }

    [NSLayoutConstraint activateConstraints:@[
        [contentRoot.widthAnchor constraintEqualToAnchor:scrollView.contentView.widthAnchor],

        [heading.leadingAnchor constraintEqualToAnchor:contentRoot.leadingAnchor constant:kPageInset],
        [heading.trailingAnchor constraintEqualToAnchor:contentRoot.trailingAnchor constant:-kPageInset],
        [heading.topAnchor constraintEqualToAnchor:contentRoot.topAnchor constant:kPageInset],

        [historyHeader.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [historyHeader.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [historyHeader.topAnchor constraintEqualToAnchor:historyTopAnchorView.bottomAnchor constant:historyTopSpacing],

        [historySection.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [historySection.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [historySection.topAnchor constraintEqualToAnchor:historyHeader.bottomAnchor constant:16.0],

        [privacyHeader.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [privacyHeader.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [privacyHeader.topAnchor constraintEqualToAnchor:historySection.bottomAnchor constant:18.0],

        [privacySection.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [privacySection.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [privacySection.topAnchor constraintEqualToAnchor:privacyHeader.bottomAnchor constant:16.0],

        [networkHeader.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [networkHeader.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [networkHeader.topAnchor constraintEqualToAnchor:privacySection.bottomAnchor constant:18.0],

        [section.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [section.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [section.topAnchor constraintEqualToAnchor:networkHeader.bottomAnchor constant:16.0],

        [hostIDHeader.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [hostIDHeader.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [hostIDHeader.topAnchor constraintEqualToAnchor:section.bottomAnchor constant:18.0],

        [hostIDSection.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [hostIDSection.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [hostIDSection.topAnchor constraintEqualToAnchor:hostIDHeader.bottomAnchor constant:16.0],

        [hostIDWarningContainer_.leadingAnchor constraintEqualToAnchor:contentRoot.leadingAnchor],
        [hostIDWarningContainer_.trailingAnchor constraintEqualToAnchor:contentRoot.trailingAnchor],
        [hostIDWarningContainer_.topAnchor constraintEqualToAnchor:hostIDSection.bottomAnchor constant:10.0],

        [hostIDWarningText_.leadingAnchor constraintEqualToAnchor:hostIDWarningContainer_.leadingAnchor constant:kPageInset],
        [hostIDWarningText_.trailingAnchor constraintEqualToAnchor:hostIDWarningContainer_.trailingAnchor constant:-kPageInset],
        [hostIDWarningText_.topAnchor constraintEqualToAnchor:hostIDWarningContainer_.topAnchor],
        [hostIDWarningText_.bottomAnchor constraintEqualToAnchor:hostIDWarningContainer_.bottomAnchor],

        [statusContainer_.leadingAnchor constraintEqualToAnchor:contentRoot.leadingAnchor],
        [statusContainer_.trailingAnchor constraintEqualToAnchor:contentRoot.trailingAnchor],
        [statusContainer_.topAnchor constraintEqualToAnchor:hostIDWarningContainer_.bottomAnchor constant:12.0],
        [statusContainer_.bottomAnchor constraintEqualToAnchor:contentRoot.bottomAnchor constant:-kPageInset],

        [statusMessage_.leadingAnchor constraintEqualToAnchor:statusContainer_.leadingAnchor constant:kPageInset],
        [statusMessage_.trailingAnchor constraintEqualToAnchor:statusContainer_.trailingAnchor constant:-kPageInset],
        [statusMessage_.topAnchor constraintEqualToAnchor:statusContainer_.topAnchor],
        [statusMessage_.bottomAnchor constraintEqualToAnchor:statusContainer_.bottomAnchor],
    ]];
    [NSLayoutConstraint activateConstraints:startupConstraints];
    [NSLayoutConstraint activateConstraints:historyConstraints];
    [NSLayoutConstraint activateConstraints:rowConstraints];
    [NSLayoutConstraint activateConstraints:privacyConstraints];
    [NSLayoutConstraint activateConstraints:hostIDConstraints];
}

void MacOSSettingsPage::LoadSettingsIntoFields() {
    if (tcpPortField_ == nil || listenerIpField_ == nil) {
        return;
    }

    loadingSettings_ = true;
    MacOSSetFieldText(tcpPortField_, g_settings.tcpPort());
    MacOSSetFieldText(listenerIpField_, g_settings.listenerIp());
    RefreshClipboardHistoryControls();
    RefreshPrivacyControls();
    RefreshLaunchAtLoginControls();
    loadingSettings_ = false;

    RefreshHostIDDisplay();
    RefreshHostIDWarning();
}

void MacOSSettingsPage::ApplyNetworkSettingChange() {
    g_networkRuntime.Restart();
    MacOSSetFieldText(statusMessage_, CLP_NS(CLP_UI_NETWORK_SETTINGS_APPLIED));
    ShowStatusMessage();
}

void MacOSSettingsPage::ShowStatusMessage() {
    if (statusContainer_ != nil) {
        statusContainer_.hidden = NO;
    }
}

void MacOSSettingsPage::ValidateTcpPort() {
    int port = 0;
    const int currentValue = g_settings.tcpPort();
    if (!uiSettingsPage::TryParsePort(MacOSToStdString(tcpPortField_.stringValue), port)) {
        MacOSSetFieldText(tcpPortField_, currentValue);
        return;
    }

    if (port != currentValue && g_settings.set_tcpPort(port)) {
        ApplyNetworkSettingChange();
    }
    MacOSSetFieldText(tcpPortField_, g_settings.tcpPort());
}

void MacOSSettingsPage::ValidateListenerIp() {
    const std::string value = uiSettingsPage::TrimAscii(MacOSToStdString(listenerIpField_.stringValue));
    const std::string currentValue = g_settings.listenerIp();
    if (!Settings::IsValidListenerIp(value)) {
        MacOSSetFieldText(listenerIpField_, currentValue);
        return;
    }

    if (value != currentValue && g_settings.set_listenerIp(value)) {
        ApplyNetworkSettingChange();
    }
    MacOSSetFieldText(listenerIpField_, g_settings.listenerIp());
}

void MacOSSettingsPage::RefreshHostIDDisplay() {
    if (hostIDValue_ == nil) {
        return;
    }

    HostId hostID;
    if (!g_settings.ensureHostID(hostID)) {
        MacOSSetFieldText(hostIDValue_, CLP_NS(CLP_UI_UNAVAILABLE));
        hostIDValue_.toolTip = @"";
        return;
    }

    NSString* text = MacOSToNSString(hostID.ToHexWString());
    MacOSSetFieldText(hostIDValue_, text);
    hostIDValue_.toolTip = text;
}

void MacOSSettingsPage::RefreshHostIDWarning() {
    if (hostIDWarningContainer_ != nil) {
        hostIDWarningContainer_.hidden = !MDNSHasHostIDCollisionWarning();
    }
}

void MacOSSettingsPage::ResetHostID() {
    HostId hostID;
    if (!g_settings.resetHostID(hostID)) {
        MacOSSetFieldText(statusMessage_, CLP_NS(CLP_UI_UNABLE_TO_RESET_HOST_ID));
        ShowStatusMessage();
        return;
    }

    g_networkRuntime.Restart();
    RefreshHostIDDisplay();
    RefreshHostIDWarning();
    MacOSSetFieldText(statusMessage_, CLP_NS(CLP_UI_HOST_ID_RESET));
    ShowStatusMessage();
}

void MacOSSettingsPage::RefreshClipboardHistoryControls() {
    if (historyMemorySlider_ == nil || historyAgeSlider_ == nil || historyItemSlider_ == nil) {
        return;
    }

    historyMemorySlider_.doubleValue = static_cast<double>(
        FindStopIndex(kHistoryMemoryStops, g_settings.clipboardHistoryMemoryLimitBytes()));
    historyAgeSlider_.doubleValue = static_cast<double>(
        FindStopIndex(kHistoryAgeStops, g_settings.clipboardHistoryMaxAgeSeconds()));
    historyItemSlider_.doubleValue = static_cast<double>(
        FindStopIndex(kHistoryItemStops, g_settings.clipboardHistoryMaxItems()));
    UpdateClipboardHistoryValueLabels();
}

void MacOSSettingsPage::UpdateClipboardHistoryValueLabels() {
    MacOSSetFieldText(historyMemoryValue_, SliderStopLabel(historyMemorySlider_, kHistoryMemoryStops));
    MacOSSetFieldText(historyAgeValue_, SliderStopLabel(historyAgeSlider_, kHistoryAgeStops));
    MacOSSetFieldText(historyItemValue_, SliderStopLabel(historyItemSlider_, kHistoryItemStops));
}

void MacOSSettingsPage::ApplyClipboardHistorySettingChange() {
    if (loadingSettings_) {
        UpdateClipboardHistoryValueLabels();
        return;
    }

    UpdateClipboardHistoryValueLabels();

    const uint64_t memoryLimitBytes = SliderStopValue(historyMemorySlider_, kHistoryMemoryStops);
    const uint64_t maxAgeSeconds = SliderStopValue(historyAgeSlider_, kHistoryAgeStops);
    const uint64_t maxItems = SliderStopValue(historyItemSlider_, kHistoryItemStops);

    bool changed = false;
    if (memoryLimitBytes != g_settings.clipboardHistoryMemoryLimitBytes()) {
        changed = g_settings.set_clipboardHistoryMemoryLimitBytes(memoryLimitBytes) || changed;
    }
    if (maxAgeSeconds != g_settings.clipboardHistoryMaxAgeSeconds()) {
        changed = g_settings.set_clipboardHistoryMaxAgeSeconds(maxAgeSeconds) || changed;
    }
    if (maxItems != g_settings.clipboardHistoryMaxItems()) {
        changed = g_settings.set_clipboardHistoryMaxItems(maxItems) || changed;
    }

    if (!changed) {
        return;
    }

    g_clipboardActivityStore.SetLimits(
        g_settings.clipboardHistoryMemoryLimitBytes(),
        g_settings.clipboardHistoryMaxAgeSeconds(),
        g_settings.clipboardHistoryMaxItems());

    MacOSSetFieldText(statusMessage_, CLP_NS(CLP_UI_CLIPBOARD_HISTORY_SETTINGS_APPLIED));
    ShowStatusMessage();
}

void MacOSSettingsPage::RefreshPrivacyControls() {
    if (honorPrivacyMarkersCheckbox_ == nil || maskShortTextPreviewsCheckbox_ == nil) {
        return;
    }

    maskShortTextPreviewsCheckbox_.state = g_settings.maskShortTextPreviews()
        ? NSControlStateValueOn
        : NSControlStateValueOff;
    honorPrivacyMarkersCheckbox_.state = g_settings.honorExternalPrivacyMarkers()
        ? NSControlStateValueOn
        : NSControlStateValueOff;
}

void MacOSSettingsPage::ApplyPrivacySettingChange() {
    if (loadingSettings_ || honorPrivacyMarkersCheckbox_ == nil || maskShortTextPreviewsCheckbox_ == nil) {
        return;
    }

    const bool desiredMask = maskShortTextPreviewsCheckbox_.state == NSControlStateValueOn;
    const bool desiredHonor = honorPrivacyMarkersCheckbox_.state == NSControlStateValueOn;

    bool changed = false;
    if (desiredMask != g_settings.maskShortTextPreviews()) {
        changed = g_settings.set_maskShortTextPreviews(desiredMask) || changed;
    }
    if (desiredHonor != g_settings.honorExternalPrivacyMarkers()) {
        changed = g_settings.set_honorExternalPrivacyMarkers(desiredHonor) || changed;
    }
    if (!changed) {
        return;
    }

    MacOSSetFieldText(statusMessage_, CLP_NS(CLP_UI_PRIVACY_SETTINGS_APPLIED));
    ShowStatusMessage();
}

void MacOSSettingsPage::RefreshLaunchAtLoginControls() {
    if (launchAtLoginCheckbox_ == nil) {
        return;
    }

    // SMAppService is the source of truth (no Settings key): it persists across
    // launches and stays honest when the user flips the login item in System
    // Settings instead of here.
    launchAtLoginCheckbox_.state = IsClippAutoStartEnabled()
        ? NSControlStateValueOn
        : NSControlStateValueOff;
}

void MacOSSettingsPage::ApplyLaunchAtLoginChange() {
    if (loadingSettings_ || launchAtLoginCheckbox_ == nil) {
        return;
    }

    const bool desired = launchAtLoginCheckbox_.state == NSControlStateValueOn;
    if (desired == IsClippAutoStartEnabled()) {
        return;
    }

    if (desired) {
        if (!RegisterClippAutoStart()) {
            // Registered but disabled by the user in System Settings ("requires
            // approval"): apps cannot programmatically re-enable it, so send the
            // user to the Login Items pane to flip it themselves.
            OpenClippLoginItemsSettings();
        }
    } else {
        UnregisterClippAutoStart();
    }

    // Reflect what actually happened rather than what was asked for.
    RefreshLaunchAtLoginControls();
}

#endif
