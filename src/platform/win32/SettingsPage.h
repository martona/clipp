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
    void ValidateUdpPort();
    void ValidateListenerIp();
    void ValidateMulticastIp();
    void RefreshHostIDDisplay();
    void RefreshHostIDWarning();
    void ResetHostID();

    static winrt::hstring ToHString(const std::string& value);

    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox tcpPortField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox udpPortField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox listenerIpField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox multicastIpField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock hostIDValue_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::Button resetHostIDButton_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock hostIDWarning_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock statusMessage_{ nullptr };
};
