#pragma once

#ifdef __APPLE__

@class NSView;
@class NSButton;
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
    void OnResetHostID();

private:
    void BuildView();
    void LoadSettingsIntoFields();
    void ApplyNetworkSettingChange();
    void ShowStatusMessage();

    void ValidateTcpPort();
    void ValidateUdpPort();
    void ValidateListenerIp();
    void ValidateMulticastIp();
    void RefreshHostIDDisplay();
    void ResetHostID();
    void RefreshHostIDWarning();

    NSView* root_ = nullptr;
    NSView* statusContainer_ = nullptr;
    NSView* hostIDWarningContainer_ = nullptr;
    NSTextField* tcpPortField_ = nullptr;
    NSTextField* udpPortField_ = nullptr;
    NSTextField* listenerIpField_ = nullptr;
    NSTextField* multicastIpField_ = nullptr;
    NSTextField* hostIDValue_ = nullptr;
    NSTextField* hostIDWarningText_ = nullptr;
    NSButton* resetHostIDButton_ = nullptr;
    NSTextField* statusMessage_ = nullptr;
    MacOSSettingsPageFieldDelegate* fieldDelegate_ = nullptr;
};

#endif
