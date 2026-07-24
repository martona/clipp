#pragma once

#include <cstdint>
#include <string>

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>

class SettingsPage {
public:
    SettingsPage();

    SettingsPage(const SettingsPage&) = delete;
    SettingsPage& operator=(const SettingsPage&) = delete;

    winrt::Windows::UI::Xaml::Controls::Grid View() const;

    void OnShown();

private:
    void BuildView();
    void LoadSettingsIntoFields();
    void ApplyNetworkSettingChange();
    void ShowStatusMessage();

    void ValidateTcpPort();
    void ValidateListenerIp();
    void RefreshHostIDDisplay();
    void RefreshHostIDWarning();
    void ResetHostID();
    void RefreshClipboardHistoryControls();
    void UpdateClipboardHistoryValueLabels();
    void ApplyClipboardHistorySettingChange();
    void RefreshPrivacyControls();
    void ApplyPrivacySettingChange();
    void RefreshFeedbackControls();
    void ApplyFeedbackSettingChange();
    void RefreshHotkeyButtonLabels();
    void BeginHotkeyCapture(int slot);
    void CancelHotkeyCapture();
    void HandleHotkeyCaptureKey(winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void CommitHotkeyChord(uint32_t chord);
    void SetHotkeyStatus(const wchar_t* text);

    static winrt::hstring ToHString(const std::string& value);

    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox tcpPortField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox listenerIpField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock hostIDValue_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::Button resetHostIDButton_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock hostIDWarning_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::Slider historyMemorySlider_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::Slider historyAgeSlider_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::Slider historyItemSlider_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock historyMemoryValue_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock historyAgeValue_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock historyItemValue_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::CheckBox maskShortTextPreviewsCheck_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::CheckBox honorPrivacyMarkersCheck_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::CheckBox animateFlowFeedbackCheck_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::Button hotkeyPrimaryButton_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::Button hotkeySecondaryButton_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock hotkeyStatus_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock statusMessage_{ nullptr };
    // -1 = not capturing; 0/1 = which slot the next keystroke re-binds.
    int capturingHotkeySlot_{ -1 };
    bool loadingSettings_{ false };
};
