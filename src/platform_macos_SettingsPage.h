#pragma once

#ifdef __APPLE__

@class NSView;
@class NSTextField;
@class MacOSSettingsPageFieldDelegate;

class MacOSSettingsPage {
public:
    MacOSSettingsPage();

    MacOSSettingsPage(const MacOSSettingsPage&) = delete;
    MacOSSettingsPage& operator=(const MacOSSettingsPage&) = delete;

    NSView* View() const;

    void OnShown();
    NSView* FirstKeyView() const;
    void ConnectKeyViewLoop(NSView* nextKeyView);
    void OnFieldEditingEnded(NSTextField* field);

private:
    void BuildView();
    void LoadSettingsIntoFields();
    void ApplyNetworkSettingChange();
    void ShowStatusMessage();

    void ValidateTcpPort();
    void ValidateUdpPort();
    void ValidateListenerIp();
    void ValidateMulticastIp();

    NSView* root_ = nullptr;
    NSView* statusContainer_ = nullptr;
    NSTextField* tcpPortField_ = nullptr;
    NSTextField* udpPortField_ = nullptr;
    NSTextField* listenerIpField_ = nullptr;
    NSTextField* multicastIpField_ = nullptr;
    NSTextField* statusMessage_ = nullptr;
    MacOSSettingsPageFieldDelegate* fieldDelegate_ = nullptr;
};

#endif
