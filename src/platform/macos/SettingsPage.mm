#include "SettingsPage.h"

#ifdef __APPLE__

#include "MDNSThread.h"
#include "NetworkRuntime.h"
#include "PeerManager.h"
#include "Settings.h"
#include "platform/uiSettingsPage.h"
#include "UiHelpers.h"

#import <AppKit/AppKit.h>

extern Settings g_settings;
extern NetworkRuntime g_networkRuntime;
extern PeerManager g_peerManager;

@interface MacOSSettingsPageFieldDelegate : NSObject <NSTextFieldDelegate> {
@private
    MacOSSettingsPage* owner_;
}
- (instancetype)initWithOwner:(MacOSSettingsPage*)owner;
- (void)resetHostID:(id)sender;
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

- (void)resetHostID:(id)sender {
    (void)sender;
    if (owner_ != nullptr) {
        owner_->OnResetHostID();
    }
}

@end

namespace {
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
        NSButton* button = [NSButton buttonWithTitle:@"Reset"
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
    return tcpPortField_;
}

void MacOSSettingsPage::ConnectKeyViewLoop(NSView* nextKeyView) {
    if (tcpPortField_ == nil || udpPortField_ == nil || listenerIpField_ == nil || multicastIpField_ == nil) {
        return;
    }

    tcpPortField_.nextKeyView = udpPortField_;
    udpPortField_.nextKeyView = listenerIpField_;
    listenerIpField_.nextKeyView = multicastIpField_;
    multicastIpField_.nextKeyView = nextKeyView;
}

void MacOSSettingsPage::OnFieldEditingEnded(NSTextField* field) {
    if (field == tcpPortField_) {
        ValidateTcpPort();
    } else if (field == udpPortField_) {
        ValidateUdpPort();
    } else if (field == listenerIpField_) {
        ValidateListenerIp();
    } else if (field == multicastIpField_) {
        ValidateMulticastIp();
    }
}

void MacOSSettingsPage::OnResetHostID() {
    ResetHostID();
}

void MacOSSettingsPage::BuildView() {
    root_ = [[NSView alloc] initWithFrame:NSZeroRect];
    root_.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField* heading = [NSTextField labelWithString:@"Settings"];
    heading.translatesAutoresizingMaskIntoConstraints = NO;
    heading.font = [NSFont systemFontOfSize:28 weight:NSFontWeightSemibold];
    heading.textColor = [NSColor labelColor];

    NSTextField* networkHeader = [NSTextField labelWithString:@"Network"];
    networkHeader.translatesAutoresizingMaskIntoConstraints = NO;
    networkHeader.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    networkHeader.textColor = [NSColor labelColor];

    NSBox* section = MacOSMakeGroupBox();

    tcpPortField_ = MacOSMakeFixedWidthTextField(110.0);
    udpPortField_ = MacOSMakeFixedWidthTextField(110.0);
    listenerIpField_ = MacOSMakeFixedWidthTextField(190.0);
    multicastIpField_ = MacOSMakeFixedWidthTextField(190.0);
    fieldDelegate_ = [[MacOSSettingsPageFieldDelegate alloc] initWithOwner:this];
    tcpPortField_.delegate = fieldDelegate_;
    udpPortField_.delegate = fieldDelegate_;
    listenerIpField_.delegate = fieldDelegate_;
    multicastIpField_.delegate = fieldDelegate_;

    NSMutableArray<NSLayoutConstraint*>* rowConstraints = [NSMutableArray array];
    AddSettingRow(section, MacOSMakeLabel(@"TCP Port"), tcpPortField_, nil, rowConstraints);
    AddSettingRow(section, MacOSMakeLabel(@"UDP Port"), udpPortField_, tcpPortField_, rowConstraints);
    AddSettingRow(section, MacOSMakeLabel(@"Listener IP"), listenerIpField_, udpPortField_, rowConstraints);
    AddSettingRow(section, MacOSMakeLabel(@"Multicast IP"), multicastIpField_, listenerIpField_, rowConstraints);
    [rowConstraints addObject:[multicastIpField_.bottomAnchor constraintEqualToAnchor:section.bottomAnchor constant:-kSectionInsetY]];

    NSTextField* hostIDHeader = [NSTextField labelWithString:@"Host ID"];
    hostIDHeader.translatesAutoresizingMaskIntoConstraints = NO;
    hostIDHeader.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    hostIDHeader.textColor = [NSColor labelColor];

    NSBox* hostIDSection = MacOSMakeGroupBox();
    hostIDValue_ = MakeHostIDValueLabel();
    resetHostIDButton_ = MakeResetButton(fieldDelegate_);

    NSMutableArray<NSLayoutConstraint*>* hostIDConstraints = [NSMutableArray array];
    AddHostIDRow(hostIDSection, MacOSMakeLabel(@"Current Host ID"), hostIDValue_, resetHostIDButton_, hostIDConstraints);

    hostIDWarningContainer_ = [[NSView alloc] initWithFrame:NSZeroRect];
    hostIDWarningContainer_.translatesAutoresizingMaskIntoConstraints = NO;
    hostIDWarningContainer_.hidden = YES;
    hostIDWarningText_ = MacOSMakeWrappingLabel(@"Possible Host ID collision detected. If this device was restored from backup or cloned, reset Host ID.",
                                                13.0,
                                                [NSColor systemOrangeColor]);
    [hostIDWarningContainer_ addSubview:hostIDWarningText_];

    [root_ addSubview:heading];
    [root_ addSubview:networkHeader];
    [root_ addSubview:section];
    [root_ addSubview:hostIDHeader];
    [root_ addSubview:hostIDSection];
    [root_ addSubview:hostIDWarningContainer_];

    statusContainer_ = [[NSView alloc] initWithFrame:NSZeroRect];
    statusContainer_.translatesAutoresizingMaskIntoConstraints = NO;
    statusContainer_.hidden = YES;

    statusMessage_ = MacOSMakeWrappingLabel(@"Network settings applied.", 13.0, [NSColor secondaryLabelColor]);
    [statusContainer_ addSubview:statusMessage_];
    [root_ addSubview:statusContainer_];

    [NSLayoutConstraint activateConstraints:@[
        [heading.leadingAnchor constraintEqualToAnchor:root_.leadingAnchor constant:kPageInset],
        [heading.trailingAnchor constraintEqualToAnchor:root_.trailingAnchor constant:-kPageInset],
        [heading.topAnchor constraintEqualToAnchor:root_.topAnchor constant:kPageInset],

        [networkHeader.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [networkHeader.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [networkHeader.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:16.0],

        [section.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [section.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [section.topAnchor constraintEqualToAnchor:networkHeader.bottomAnchor constant:16.0],

        [hostIDHeader.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [hostIDHeader.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [hostIDHeader.topAnchor constraintEqualToAnchor:section.bottomAnchor constant:18.0],

        [hostIDSection.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor],
        [hostIDSection.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [hostIDSection.topAnchor constraintEqualToAnchor:hostIDHeader.bottomAnchor constant:16.0],

        [hostIDWarningContainer_.leadingAnchor constraintEqualToAnchor:root_.leadingAnchor],
        [hostIDWarningContainer_.trailingAnchor constraintEqualToAnchor:root_.trailingAnchor],
        [hostIDWarningContainer_.topAnchor constraintEqualToAnchor:hostIDSection.bottomAnchor constant:10.0],

        [hostIDWarningText_.leadingAnchor constraintEqualToAnchor:hostIDWarningContainer_.leadingAnchor constant:kPageInset],
        [hostIDWarningText_.trailingAnchor constraintEqualToAnchor:hostIDWarningContainer_.trailingAnchor constant:-kPageInset],
        [hostIDWarningText_.topAnchor constraintEqualToAnchor:hostIDWarningContainer_.topAnchor],
        [hostIDWarningText_.bottomAnchor constraintEqualToAnchor:hostIDWarningContainer_.bottomAnchor],

        [statusContainer_.leadingAnchor constraintEqualToAnchor:root_.leadingAnchor],
        [statusContainer_.trailingAnchor constraintEqualToAnchor:root_.trailingAnchor],
        [statusContainer_.topAnchor constraintEqualToAnchor:hostIDWarningContainer_.bottomAnchor constant:12.0],

        [statusMessage_.leadingAnchor constraintEqualToAnchor:statusContainer_.leadingAnchor constant:kPageInset],
        [statusMessage_.trailingAnchor constraintEqualToAnchor:statusContainer_.trailingAnchor constant:-kPageInset],
        [statusMessage_.topAnchor constraintEqualToAnchor:statusContainer_.topAnchor],
        [statusMessage_.bottomAnchor constraintEqualToAnchor:statusContainer_.bottomAnchor],
    ]];
    [NSLayoutConstraint activateConstraints:rowConstraints];
    [NSLayoutConstraint activateConstraints:hostIDConstraints];
}

void MacOSSettingsPage::LoadSettingsIntoFields() {
    if (tcpPortField_ == nil || udpPortField_ == nil || listenerIpField_ == nil || multicastIpField_ == nil) {
        return;
    }

    MacOSSetFieldText(tcpPortField_, g_settings.tcpPort());
    MacOSSetFieldText(udpPortField_, g_settings.mdnsPort());
    MacOSSetFieldText(listenerIpField_, g_settings.listenerIp());
    MacOSSetFieldText(multicastIpField_, g_settings.multicastIp());
    RefreshHostIDDisplay();
    RefreshHostIDWarning();
}

void MacOSSettingsPage::ApplyNetworkSettingChange() {
    g_networkRuntime.Restart();
    MacOSSetFieldText(statusMessage_, @"Network settings applied.");
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

void MacOSSettingsPage::ValidateUdpPort() {
    int port = 0;
    const int currentValue = g_settings.mdnsPort();
    if (!uiSettingsPage::TryParsePort(MacOSToStdString(udpPortField_.stringValue), port)) {
        MacOSSetFieldText(udpPortField_, currentValue);
        return;
    }

    if (port != currentValue && g_settings.set_mdnsPort(port)) {
        ApplyNetworkSettingChange();
    }
    MacOSSetFieldText(udpPortField_, g_settings.mdnsPort());
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

void MacOSSettingsPage::ValidateMulticastIp() {
    const std::string value = uiSettingsPage::TrimAscii(MacOSToStdString(multicastIpField_.stringValue));
    const std::string currentValue = g_settings.multicastIp();
    if (!Settings::IsValidMulticastIp(value)) {
        MacOSSetFieldText(multicastIpField_, currentValue);
        return;
    }

    if (value != currentValue && g_settings.set_multicastIp(value)) {
        ApplyNetworkSettingChange();
    }
    MacOSSetFieldText(multicastIpField_, g_settings.multicastIp());
}

void MacOSSettingsPage::RefreshHostIDDisplay() {
    if (hostIDValue_ == nil) {
        return;
    }

    HostId hostID;
    if (!g_settings.ensureHostID(hostID)) {
        MacOSSetFieldText(hostIDValue_, @"Unavailable");
        hostIDValue_.toolTip = @"";
        return;
    }

    NSString* text = MacOSToNSString(hostID.ToHexWString(8));
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
        MacOSSetFieldText(statusMessage_, @"Unable to reset Host ID.");
        ShowStatusMessage();
        return;
    }

    MDNSNotifyHostIDChange();
    g_peerManager.ClearPeers();
    RefreshHostIDDisplay();
    RefreshHostIDWarning();
    MacOSSetFieldText(statusMessage_, @"Host ID reset.");
    ShowStatusMessage();
}

#endif
