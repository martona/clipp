#include "platform_macos_SettingsPage.h"

#ifdef __APPLE__

#include "NetworkRuntime.h"
#include "Settings.h"
#include "platform/uiSettingsPage.h"

#import <AppKit/AppKit.h>

extern Settings g_settings;
extern NetworkRuntime g_networkRuntime;

namespace {
    constexpr CGFloat kPageInset = 28.0;
    constexpr CGFloat kSectionInsetX = 18.0;
    constexpr CGFloat kSectionInsetY = 14.0;

    static NSString* ToNSString(const std::string& value) {
        NSString* text = [NSString stringWithUTF8String:value.c_str()];
        return text != nil ? text : @"";
    }

    static std::string ToStdString(NSString* value) {
        if (value == nil) {
            return {};
        }

        const char* utf8 = value.UTF8String;
        return utf8 != nullptr ? std::string(utf8) : std::string();
    }

    static NSTextField* MakeLabel(NSString* text) {
        NSTextField* label = [NSTextField labelWithString:text];
        label.translatesAutoresizingMaskIntoConstraints = NO;
        label.font = [NSFont systemFontOfSize:13];
        label.textColor = [NSColor labelColor];
        return label;
    }

    static NSTextField* MakeTextField(CGFloat width) {
        NSTextField* field = [NSTextField textFieldWithString:@""];
        field.translatesAutoresizingMaskIntoConstraints = NO;
        field.font = [NSFont systemFontOfSize:13];
        field.alignment = NSTextAlignmentLeft;
        [field.widthAnchor constraintEqualToConstant:width].active = YES;
        return field;
    }

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

    static void SetFieldText(NSTextField* field, const std::string& value) {
        if (field != nil) {
            field.stringValue = ToNSString(value);
        }
    }

    static void SetFieldText(NSTextField* field, int value) {
        if (field != nil) {
            field.stringValue = [NSString stringWithFormat:@"%d", value];
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

    NSBox* section = [[NSBox alloc] initWithFrame:NSZeroRect];
    section.translatesAutoresizingMaskIntoConstraints = NO;
    section.boxType = NSBoxCustom;
    section.titlePosition = NSNoTitle;
    section.borderType = NSNoBorder;
    section.cornerRadius = 10.0;
    section.fillColor = [NSColor alternatingContentBackgroundColors][1];

    tcpPortField_ = MakeTextField(110.0);
    udpPortField_ = MakeTextField(110.0);
    listenerIpField_ = MakeTextField(190.0);
    multicastIpField_ = MakeTextField(190.0);

    NSMutableArray<NSLayoutConstraint*>* rowConstraints = [NSMutableArray array];
    AddSettingRow(section, MakeLabel(@"TCP Port"), tcpPortField_, nil, rowConstraints);
    AddSettingRow(section, MakeLabel(@"UDP Port"), udpPortField_, tcpPortField_, rowConstraints);
    AddSettingRow(section, MakeLabel(@"Listener IP"), listenerIpField_, udpPortField_, rowConstraints);
    AddSettingRow(section, MakeLabel(@"Multicast IP"), multicastIpField_, listenerIpField_, rowConstraints);
    [rowConstraints addObject:[multicastIpField_.bottomAnchor constraintEqualToAnchor:section.bottomAnchor constant:-kSectionInsetY]];

    [root_ addSubview:heading];
    [root_ addSubview:networkHeader];
    [root_ addSubview:section];

    statusContainer_ = [[NSView alloc] initWithFrame:NSZeroRect];
    statusContainer_.translatesAutoresizingMaskIntoConstraints = NO;
    statusContainer_.hidden = YES;

    statusMessage_ = [NSTextField wrappingLabelWithString:@"Network settings applied."];
    statusMessage_.translatesAutoresizingMaskIntoConstraints = NO;
    statusMessage_.font = [NSFont systemFontOfSize:13];
    statusMessage_.textColor = [NSColor secondaryLabelColor];
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

    SetFieldText(tcpPortField_, g_settings.tcpPort());
    SetFieldText(udpPortField_, g_settings.mdnsPort());
    SetFieldText(listenerIpField_, g_settings.listenerIp());
    SetFieldText(multicastIpField_, g_settings.multicastIp());
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
    if (!uiSettingsPage::TryParsePort(ToStdString(tcpPortField_.stringValue), port)) {
        SetFieldText(tcpPortField_, currentValue);
        return;
    }

    if (port != currentValue && g_settings.set_tcpPort(port)) {
        ApplyNetworkSettingChange();
    }
    SetFieldText(tcpPortField_, g_settings.tcpPort());
}

void MacOSSettingsPage::ValidateUdpPort() {
    int port = 0;
    const int currentValue = g_settings.mdnsPort();
    if (!uiSettingsPage::TryParsePort(ToStdString(udpPortField_.stringValue), port)) {
        SetFieldText(udpPortField_, currentValue);
        return;
    }

    if (port != currentValue && g_settings.set_mdnsPort(port)) {
        ApplyNetworkSettingChange();
    }
    SetFieldText(udpPortField_, g_settings.mdnsPort());
}

void MacOSSettingsPage::ValidateListenerIp() {
    const std::string value = uiSettingsPage::TrimAscii(ToStdString(listenerIpField_.stringValue));
    const std::string currentValue = g_settings.listenerIp();
    if (!Settings::IsValidListenerIp(value)) {
        SetFieldText(listenerIpField_, currentValue);
        return;
    }

    if (value != currentValue && g_settings.set_listenerIp(value)) {
        ApplyNetworkSettingChange();
    }
    SetFieldText(listenerIpField_, g_settings.listenerIp());
}

void MacOSSettingsPage::ValidateMulticastIp() {
    const std::string value = uiSettingsPage::TrimAscii(ToStdString(multicastIpField_.stringValue));
    const std::string currentValue = g_settings.multicastIp();
    if (!Settings::IsValidMulticastIp(value)) {
        SetFieldText(multicastIpField_, currentValue);
        return;
    }

    if (value != currentValue && g_settings.set_multicastIp(value)) {
        ApplyNetworkSettingChange();
    }
    SetFieldText(multicastIpField_, g_settings.multicastIp());
}

#endif
