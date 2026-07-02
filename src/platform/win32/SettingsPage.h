#pragma once

#include <string>

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

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
    winrt::Windows::UI::Xaml::Controls::TextBlock statusMessage_{ nullptr };
    bool loadingSettings_{ false };
};
