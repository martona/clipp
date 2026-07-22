#pragma once

#ifdef __APPLE__

@class NSView;
@class NSButton;
@class NSSlider;
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
    void OnHistorySliderChanged();
    void OnResetHostID();
    void OnHonorPrivacyMarkersChanged();
    void OnMaskShortTextPreviewsChanged();
    void OnAnimateFlowFeedbackChanged();
    void OnLaunchAtLoginChanged();

private:
    void BuildView();
    void LoadSettingsIntoFields();
    void ApplyNetworkSettingChange();
    void ShowStatusMessage();

    void ValidateTcpPort();
    void ValidateListenerIp();
    void RefreshHostIDDisplay();
    void ResetHostID();
    void RefreshHostIDWarning();
    void RefreshClipboardHistoryControls();
    void UpdateClipboardHistoryValueLabels();
    void ApplyClipboardHistorySettingChange();
    void RefreshPrivacyControls();
    void ApplyPrivacySettingChange();
    void RefreshFeedbackControls();
    void ApplyFeedbackSettingChange();
    void RefreshLaunchAtLoginControls();
    void ApplyLaunchAtLoginChange();

    NSView* root_ = nullptr;
    NSView* statusContainer_ = nullptr;
    NSView* hostIDWarningContainer_ = nullptr;
    NSTextField* tcpPortField_ = nullptr;
    NSTextField* listenerIpField_ = nullptr;
    NSSlider* historyMemorySlider_ = nullptr;
    NSSlider* historyAgeSlider_ = nullptr;
    NSSlider* historyItemSlider_ = nullptr;
    NSTextField* historyMemoryValue_ = nullptr;
    NSTextField* historyAgeValue_ = nullptr;
    NSTextField* historyItemValue_ = nullptr;
    NSTextField* hostIDValue_ = nullptr;
    NSTextField* hostIDWarningText_ = nullptr;
    NSButton* resetHostIDButton_ = nullptr;
    NSButton* maskShortTextPreviewsCheckbox_ = nullptr;
    NSButton* honorPrivacyMarkersCheckbox_ = nullptr;
    NSButton* animateFlowFeedbackCheckbox_ = nullptr;
    NSButton* launchAtLoginCheckbox_ = nullptr;
    NSTextField* statusMessage_ = nullptr;
    MacOSSettingsPageFieldDelegate* fieldDelegate_ = nullptr;
    bool loadingSettings_ = false;
};

#endif
