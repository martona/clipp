#pragma once

#include "PeerDisplay.h"

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include <winrt/Windows.UI.Xaml.Controls.h>

class NetworkItemView {
public:
    explicit NetworkItemView(const PeerDisplayItem& item);

    winrt::Windows::UI::Xaml::Controls::StackPanel View() const;

    void UpdateHostName(const std::wstring& hostName);
    void UpdateHostID(const HostId& hostID);
    void UpdateIncomingConnection(bool connected);
    void UpdateOutgoingConnection(bool connected);
    void UpdateBytesSent(uint64_t bytesSent);
    void UpdateBytesReceived(uint64_t bytesReceived);
    void UpdateConnectedSince(std::chrono::steady_clock::time_point connectedSince);
    void RefreshConnectedFor(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

private:
    static std::wstring DisplayHostName(const std::wstring& hostName);
    static std::wstring FormatHostID(const std::array<unsigned char, 32>& hostID);
    static std::wstring FormatByteCounter(uint64_t bytes);
    static std::wstring FormatConnectionState(bool connected);
    static std::wstring FormatConnectedFor(std::chrono::steady_clock::time_point connectedSince, std::chrono::steady_clock::time_point now);

    winrt::Windows::UI::Xaml::Controls::StackPanel card_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock title_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock subtitle_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::FontIcon inIcon_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::FontIcon outIcon_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock hostIDValue_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock bytesSentValue_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock bytesReceivedValue_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock incomingValue_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock outgoingValue_{ nullptr };

    std::chrono::steady_clock::time_point connectedSince_{};
    std::wstring connectedForText_;
};
