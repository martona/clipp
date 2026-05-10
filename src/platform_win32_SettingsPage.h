#pragma once

#include <string>

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
    void ShowRestartMessage();

    void ValidateTcpPort();
    void ValidateUdpPort();
    void ValidateListenerIp();
    void ValidateMulticastIp();

    static winrt::hstring ToHString(const std::string& value);
    static std::string TrimAscii(std::string value);
    static bool TryParsePort(const winrt::hstring& text, int& port);
    static bool IsValidListenerIp(const std::string& value);
    static bool IsValidMulticastIp(const std::string& value);

    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox tcpPortField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox udpPortField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox listenerIpField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox multicastIpField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock restartMessage_{ nullptr };
};
