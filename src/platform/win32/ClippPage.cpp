#include "ClippPage.h"

#include "KeyManager.h"
#include "Logger.h"
#include "MDNSThread.h"
#include "Settings.h"
#include "platform/uiClippPage.h"
#include "utils.h"

#include <string>

#include <sodium.h>

#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

extern Settings g_settings;
extern KeyManager g_keyManager;
extern Logger g_logger;

namespace {
constexpr wchar_t kMaskedPassword[] = L"••••••••••••••••";
}

ClippPage::ClippPage(HWND notificationTarget, UINT derivedKeyMessage, UINT peerDisplayUpdateMessage, PeerDisplay& peerDisplay, PeerManager& peerManager)
    : notificationTarget_(notificationTarget)
    , derivedKeyMessage_(derivedKeyMessage)
    , peerDisplayUpdateMessage_(peerDisplayUpdateMessage)
    , peerDisplay_(peerDisplay)
    , peerManager_(peerManager)
    , keyDerivationWorker_([this](const KeyManager::NetworkKey& key) {
        PostDerivedKey(key);
    }) {
    BuildView();
}

ClippPage::~ClippPage() {
    OnDestroy();
}

winrt::Windows::UI::Xaml::Controls::Grid ClippPage::View() const {
    return root_;
}

void ClippPage::BuildView() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    root_ = Grid();
    root_.HorizontalAlignment(HorizontalAlignment::Stretch);
    root_.VerticalAlignment(VerticalAlignment::Stretch);

    StackPanel content;
    content.Orientation(Orientation::Vertical);
    content.Padding(ThicknessHelper::FromUniformLength(24));
    content.Spacing(16);

    TextBlock heading;
    heading.Text(L"Clipp");
    heading.FontSize(28);
    heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    heading.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(heading);

    TextBlock intro;
    intro.Text(L"Secure cross-platform clipboard sync for trusted devices.");
    intro.FontSize(14);
    intro.TextWrapping(TextWrapping::WrapWholeWords);
    content.Children().Append(intro);

    BuildNetworkSecretSection(content);

    networkView_ = std::make_unique<NetworkView>(peerDisplay_);
    content.Children().Append(networkView_->View());
    PollNetworkView();

    ScrollViewer mainScroll;
    mainScroll.HorizontalAlignment(HorizontalAlignment::Stretch);
    mainScroll.VerticalAlignment(VerticalAlignment::Stretch);
    mainScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    mainScroll.Content(content);
    root_.Children().Append(mainScroll);

    uiDispatcher_ = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
}

void ClippPage::BuildNetworkSecretSection(winrt::Windows::UI::Xaml::Controls::StackPanel const& content) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Media;

    TextBlock passwordHeader;
    passwordHeader.Text(L"Network");
    passwordHeader.FontSize(16);
    passwordHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());

    TextBlock networkNameLabel;
    networkNameLabel.Text(L"Name");
    networkNameLabel.VerticalAlignment(VerticalAlignment::Center);

    TextBlock passwordLabel;
    passwordLabel.Text(L"Secret");
    passwordLabel.VerticalAlignment(VerticalAlignment::Center);

    networkNameField_ = TextBox();
    networkNameField_.VerticalAlignment(VerticalAlignment::Center);

    const std::string currentName = g_settings.networkName();
    const size_t sizeNeeded = utf8_to_utf16(currentName.c_str(), currentName.length(), nullptr, 0);
    std::wstring wideCurrentName(sizeNeeded, 0);
    utf8_to_utf16(currentName.c_str(), currentName.length(), &wideCurrentName[0], sizeNeeded);
    networkNameField_.Text(wideCurrentName);

    networkNameField_.LostFocus([this](auto const& sender, auto const&) {
        auto tb = sender.as<TextBox>();
        const std::string newName = winrt::to_string(tb.Text());

        if (newName != g_settings.networkName() && !newName.empty()) {
            g_settings.set_networkName(newName);
            g_keyManager.ClearNetworkKey();
            MDNSNotifyNetworkKeyChange();
            peerManager_.ClearPeers();
            SetupPasswordFields();
        }
    });

    DispatcherTimer debounceTimer;
    debounceTimer.Interval(std::chrono::milliseconds(500));
    debounceTimer.Stop();
    debounceTimer.Tick([this, debounceTimer](auto const&, auto const&) {
        debounceTimer.Stop();
        winrt::hstring pwd = passwordField_.Password();

        passwordStatusPanel_.Visibility(Visibility::Collapsed);
        passwordInfoPanel_.Visibility(Visibility::Visible);

        if (pwd.size() < 8) {
            passwordInfoText_.Text(L"Secret must be at least 8 characters.");
            return;
        }

        passwordInfoText_.Text(L"... working ...");

        const std::string networkName = g_settings.networkName();
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "Generating key with secret (network name: %s)", networkName.c_str());
        std::string newPassword = winrt::to_string(pwd);
        std::string netNameAndPassword = uiClippPage::BuildKeyDerivationInput(networkName, newPassword);
        keyDerivationWorker_.RequestKeyDerivation(netNameAndPassword);
        sodium_memzero(newPassword.data(), newPassword.capacity());
        sodium_memzero(netNameAndPassword.data(), netNameAndPassword.capacity());
    });

    passwordField_ = PasswordBox();
    passwordField_.VerticalAlignment(VerticalAlignment::Center);
    passwordField_.MinWidth(200);
    passwordField_.Tag(winrt::box_value(false));
    passwordField_.GotFocus([this](auto const&, auto const&) {
        passwordField_.Password(L"");
    });
    passwordField_.LostFocus([this](auto const&, auto const&) {
        SetupPasswordFields();
    });
    passwordField_.PasswordChanged([this, debounceTimer](auto const&, auto const&) {
        debounceTimer.Stop();
        if (passwordField_.Password() != L"" && passwordField_.Password() != kMaskedPassword) {
            debounceTimer.Start();
        }
    });

    Grid inputGrid;
    inputGrid.CornerRadius(CornerRadius{ 4 });
    inputGrid.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    inputGrid.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));
    inputGrid.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    inputGrid.RowSpacing(12);
    inputGrid.ColumnSpacing(16);

    ColumnDefinition col1, col2;
    col1.Width(GridLength{ 1, GridUnitType::Auto });
    col2.Width(GridLength{ 1, GridUnitType::Star });
    inputGrid.ColumnDefinitions().Append(col1);
    inputGrid.ColumnDefinitions().Append(col2);

    RowDefinition row1, row2;
    row1.Height(GridLength{ 1, GridUnitType::Auto });
    row2.Height(GridLength{ 1, GridUnitType::Auto });
    inputGrid.RowDefinitions().Append(row1);
    inputGrid.RowDefinitions().Append(row2);

    Grid::SetRow(networkNameLabel, 0);
    Grid::SetColumn(networkNameLabel, 0);
    Grid::SetRow(networkNameField_, 0);
    Grid::SetColumn(networkNameField_, 1);
    Grid::SetRow(passwordLabel, 1);
    Grid::SetColumn(passwordLabel, 0);
    Grid::SetRow(passwordField_, 1);
    Grid::SetColumn(passwordField_, 1);

    inputGrid.Children().Append(networkNameLabel);
    inputGrid.Children().Append(networkNameField_);
    inputGrid.Children().Append(passwordLabel);
    inputGrid.Children().Append(passwordField_);

    passwordStatusPanel_ = StackPanel();
    passwordStatusPanel_.CornerRadius(CornerRadius{ 4 });
    passwordStatusPanel_.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    passwordStatusPanel_.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));
    passwordStatusPanel_.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    passwordStatusPanel_.Orientation(Orientation::Horizontal);
    passwordStatusPanel_.Spacing(12);

    FontIcon keyIcon;
    keyIcon.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
    keyIcon.Glyph(L"\xE8D7");
    keyIcon.FontSize(18);
    keyIcon.VerticalAlignment(VerticalAlignment::Center);

    StackPanel statusTextStack;
    statusTextStack.Orientation(Orientation::Vertical);
    statusTextStack.Spacing(2);
    statusTextStack.VerticalAlignment(VerticalAlignment::Center);

    passwordHashText_ = TextBlock();
    passwordHashText_.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
    passwordHashText_.TextWrapping(TextWrapping::Wrap);

    TextBlock hashExplainer;
    hashExplainer.Text(L"Network key fingerprint. Used only on this screen; not in itself a secret.");
    hashExplainer.Opacity(0.6);
    hashExplainer.TextWrapping(TextWrapping::Wrap);

    statusTextStack.Children().Append(passwordHashText_);
    statusTextStack.Children().Append(hashExplainer);
    passwordStatusPanel_.Children().Append(keyIcon);
    passwordStatusPanel_.Children().Append(statusTextStack);

    passwordInfoPanel_ = StackPanel();
    passwordInfoPanel_.CornerRadius(CornerRadius{ 4 });
    passwordInfoPanel_.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    passwordInfoPanel_.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));
    passwordInfoPanel_.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    passwordInfoPanel_.Orientation(Orientation::Horizontal);
    passwordInfoPanel_.Spacing(12);

    FontIcon infoIcon;
    infoIcon.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
    infoIcon.Glyph(L"\xE946");
    infoIcon.FontSize(18);
    infoIcon.VerticalAlignment(VerticalAlignment::Center);

    passwordInfoText_ = TextBlock();
    passwordInfoText_.VerticalAlignment(VerticalAlignment::Center);

    passwordInfoPanel_.Children().Append(infoIcon);
    passwordInfoPanel_.Children().Append(passwordInfoText_);

    StackPanel outerContainer;
    outerContainer.Orientation(Orientation::Vertical);
    outerContainer.Spacing(10);
    outerContainer.Children().Append(passwordHeader);
    outerContainer.Children().Append(inputGrid);
    outerContainer.Children().Append(passwordStatusPanel_);
    outerContainer.Children().Append(passwordInfoPanel_);

    content.Children().Append(outerContainer);
    SetupPasswordFields();
}

void ClippPage::OnShown() {
    BeginPeerNotifications();
    StartNetworkPollTimer();
    if (uiDispatcher_) {
        uiDispatcher_.TryEnqueue([this]() {
            SetupPasswordFields();
        });
    }
}

void ClippPage::OnHidden() {
    EndPeerNotifications();
    StopNetworkPollTimer();
}

void ClippPage::OnDestroy() {
    OnHidden();
    networkView_.reset();
    networkPollTimer_ = nullptr;
}

void ClippPage::OnDerivedKey(const KeyManager::NetworkKey* key) {
    if (!key) {
        return;
    }

    g_settings.set_networkName(g_settings.networkName());
    g_keyManager.SetNetworkKey(*key);
    MDNSNotifyNetworkKeyChange();
    peerManager_.ClearPeers();

    if (uiDispatcher_) {
        uiDispatcher_.TryEnqueue([this]() {
            NewPasswordHashReceived();
        });
    }
}

void ClippPage::OnPeerDisplayUpdate() {
    if (peerDisplayWatcherID_ != 0) {
        PollNetworkView();
    }
}

void ClippPage::PollNetworkView() {
    if (networkView_) {
        networkView_->Poll();
    }
}

void ClippPage::StartNetworkPollTimer() {
    if (!networkPollTimer_) {
        networkPollTimer_ = winrt::Windows::UI::Xaml::DispatcherTimer();
        networkPollTimer_.Interval(std::chrono::seconds(1));
        networkPollTimer_.Tick([this](auto const&, auto const&) {
            PollNetworkView();
        });
    }
    networkPollTimer_.Start();
}

void ClippPage::StopNetworkPollTimer() {
    if (networkPollTimer_) {
        networkPollTimer_.Stop();
    }
}

void ClippPage::BeginPeerNotifications() {
    if (!notificationTarget_ || peerDisplayWatcherID_ != 0) {
        return;
    }

    const auto registration = peerDisplay_.QueryAndRegister(PeerDisplayWatcher, this);
    peerDisplayWatcherID_ = registration.watcherID;
    PollNetworkView();
}

void ClippPage::EndPeerNotifications() {
    if (peerDisplayWatcherID_ == 0) {
        return;
    }

    peerDisplay_.Unregister(peerDisplayWatcherID_);
    peerDisplayWatcherID_ = 0;
}

void ClippPage::SetupPasswordFields() {
    using namespace winrt::Windows::UI::Xaml;

    if (!passwordField_ || !passwordHashText_ || !passwordStatusPanel_ || !passwordInfoPanel_ || !passwordInfoText_) {
        return;
    }

    if (g_keyManager.HaveNetworkKey()) {
        passwordField_.Password(kMaskedPassword);
        passwordHashText_.Text(g_keyManager.GetNetworkFingerprintHash());
        passwordStatusPanel_.Visibility(Visibility::Visible);
        passwordInfoPanel_.Visibility(Visibility::Collapsed);
    } else {
        passwordField_.Password(L"");
        passwordInfoText_.Text(L"Enter network secret to create or join a network.");
        passwordStatusPanel_.Visibility(Visibility::Collapsed);
        passwordInfoPanel_.Visibility(Visibility::Visible);
    }
}

void ClippPage::NewPasswordHashReceived() {
    using namespace winrt::Windows::UI::Xaml;

    if (g_keyManager.HaveNetworkKey()) {
        passwordHashText_.Text(g_keyManager.GetNetworkFingerprintHash());
        passwordStatusPanel_.Visibility(Visibility::Visible);
        passwordInfoPanel_.Visibility(Visibility::Collapsed);
    }
}

void ClippPage::PostDerivedKey(const KeyManager::NetworkKey& key) {
    if (notificationTarget_ == nullptr || derivedKeyMessage_ == 0) {
        return;
    }

    SendMessage(notificationTarget_, derivedKeyMessage_, reinterpret_cast<WPARAM>(&key), 0);
}

void ClippPage::PeerDisplayWatcher(const PeerDisplayUpdate&, void* userData) {
    auto* page = reinterpret_cast<ClippPage*>(userData);
    if (page && page->notificationTarget_ && IsWindow(page->notificationTarget_)) {
        PostMessageW(page->notificationTarget_, page->peerDisplayUpdateMessage_, 0, 0);
    }
}
