#include "SettingsPage.h"

#ifdef __APPLE__

#include "NetworkRuntime.h"
#include "Settings.h"
#include "platform/uiSettingsPage.h"
#include "UiHelpers.h"

#import <AppKit/AppKit.h>

extern Settings g_settings;
extern NetworkRuntime g_networkRuntime;

@interface MacOSSettingsPageFieldDelegate : NSObject <NSTextFieldDelegate> {
@private
    MacOSSettingsPage* owner_;
}
- (instancetype)initWithOwner:(MacOSSettingsPage*)owner;
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

    [root_ addSubview:heading];
    [root_ addSubview:networkHeader];
    [root_ addSubview:section];

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

        [statusContainer_.leadingAnchor constraintEqualToAnchor:root_.leadingAnchor],
        [statusContainer_.trailingAnchor constraintEqualToAnchor:root_.trailingAnchor],
        [statusContainer_.topAnchor constraintEqualToAnchor:section.bottomAnchor constant:12.0],

        [statusMessage_.leadingAnchor constraintEqualToAnchor:statusContainer_.leadingAnchor constant:kPageInset],
        [statusMessage_.trailingAnchor constraintEqualToAnchor:statusContainer_.trailingAnchor constant:-kPageInset],
        [statusMessage_.topAnchor constraintEqualToAnchor:statusContainer_.topAnchor],
        [statusMessage_.bottomAnchor constraintEqualToAnchor:statusContainer_.bottomAnchor],
    ]];
    [NSLayoutConstraint activateConstraints:rowConstraints];
}

void MacOSSettingsPage::LoadSettingsIntoFields() {
    if (tcpPortField_ == nil || udpPortField_ == nil || listenerIpField_ == nil || multicastIpField_ == nil) {
        return;
    }

    MacOSSetFieldText(tcpPortField_, g_settings.tcpPort());
    MacOSSetFieldText(udpPortField_, g_settings.mdnsPort());
    MacOSSetFieldText(listenerIpField_, g_settings.listenerIp());
    MacOSSetFieldText(multicastIpField_, g_settings.multicastIp());
}

void MacOSSettingsPage::ApplyNetworkSettingChange() {
    g_networkRuntime.Restart();
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

#endif
