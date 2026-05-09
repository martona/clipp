#pragma once

#include "PeerDisplay.h"
#include "PeerManager.h"
#include "platform_win32_KeyDerivationWorker.h"
#include "platform_win32_NetworkView.h"
#include "platform_win32_TerminalLogView.h"

#include <memory>
#include <mutex>

#include <Windows.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

class ClippPage {
public:
    ClippPage(HWND notificationTarget, UINT derivedKeyMessage, UINT peerDisplayUpdateMessage, PeerDisplay& peerDisplay, PeerManager& peerManager);
    ~ClippPage();

    ClippPage(const ClippPage&) = delete;
    ClippPage& operator=(const ClippPage&) = delete;

    winrt::Windows::UI::Xaml::Controls::Grid View() const;

    void OnShown();
    void OnHidden();
    void OnDestroy();
    void OnDerivedKey(KeyDerivationWorker::KeyDerivationResult* result);
    void OnPeerDisplayUpdate();

private:
    void BuildView();
    void BuildNetworkSecretSection(winrt::Windows::UI::Xaml::Controls::StackPanel const& content);
    winrt::Windows::UI::Xaml::Controls::ScrollViewer CreateTerminalLikeScrollViewer();

    void PollNetworkView();
    void StartNetworkPollTimer();
    void StopNetworkPollTimer();
    void BeginPeerNotifications();
    void EndPeerNotifications();
    void SetupPasswordFields();
    void NewPasswordHashReceived();
    void ReflectLogLine(const std::wstring& line);

    static void PeerDisplayWatcher(const PeerDisplayUpdate& update, void* userData);
    static void LogReflectorCallback(const std::wstring& line);

    HWND notificationTarget_ = nullptr;
    UINT derivedKeyMessage_ = 0;
    UINT peerDisplayUpdateMessage_ = 0;
    PeerDisplay& peerDisplay_;
    PeerManager& peerManager_;

    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBox networkNameField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::PasswordBox passwordField_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::StackPanel passwordStatusPanel_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock passwordHashText_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::StackPanel passwordInfoPanel_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock passwordInfoText_{ nullptr };
    winrt::Windows::System::DispatcherQueue uiDispatcher_{ nullptr };
    winrt::Windows::UI::Xaml::DispatcherTimer networkPollTimer_{ nullptr };

    std::unique_ptr<TerminalLogView> terminalLogView_;
    std::unique_ptr<NetworkView> networkView_;
    std::mutex terminalLogViewMutex_;
    KeyDerivationWorker keyDerivationWorker_;
    std::size_t peerDisplayWatcherID_ = 0;
    bool logReflectorRegistered_ = false;
};
