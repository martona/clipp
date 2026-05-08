#include "PeerManager.h"
#include "PeerDisplay.h"
#include "platform_win32_xaml_dialog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <Windows.h>
// winrt/Windows.UI.Xaml.Media.Animation.h has a GetCurrentTime method; Windows.h
// also defines GetCurrentTime as a compatibility macro, which breaks C++/WinRT.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <dwmapi.h>
#include <unknwn.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.System.h>
#include <winrt/base.h>

#include "Logger.h"
#include "platform_win32_TerminalLogView.h"
#include "MDNSThread.h"
#include "clipp-win32-darkmode32/DMSubclass.h"
#include "platform_win32_KeyDerivationWorker.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "windowsapp.lib")

extern PeerManager g_peerManager;
extern PeerDisplay g_peerDisplay;

namespace {

constexpr wchar_t kDialogClassName[] = L"ClippMainXamlDialog";
constexpr double kDialogDefaultClientWidthDips = 720;
constexpr double kDialogDefaultClientHeightDips = 560;
constexpr double kDialogMinClientWidthDips = 520;
constexpr double kDialogMinClientHeightDips = 420;

struct XamlDialogState {
    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource xamlSource{ nullptr };
    HWND xamlHost = nullptr;
};

HWND g_dialogWindow = nullptr;
winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager g_xamlManager{ nullptr };
std::wstring g_createError;

winrt::Windows::UI::Xaml::Controls::Button g_changeBtn{ nullptr };
winrt::Windows::UI::Xaml::Controls::PasswordBox g_passwordField{ nullptr };
winrt::Windows::UI::Xaml::Controls::TextBlock g_readOnlyText{ nullptr };
//winrt::Windows::UI::Xaml::Controls::TextBlock g_hashDisplay{ nullptr };
winrt::Windows::UI::Xaml::Controls::TextBox g_peerDisplayTextBox{ nullptr };
winrt::Windows::System::DispatcherQueue g_uiDispatcher{ nullptr };
std::unique_ptr<TerminalLogView> g_terminalLogView;
std::mutex g_terminalLogViewMutex;
winrt::Windows::UI::Xaml::Controls::StackPanel g_passwordStatusPanel{ nullptr };
winrt::Windows::UI::Xaml::Controls::TextBlock g_passwordHashText{ nullptr };
winrt::Windows::UI::Xaml::Controls::StackPanel g_passwordInfoPanel{ nullptr };
winrt::Windows::UI::Xaml::Controls::TextBlock g_passwordInfoText{ nullptr };

KeyDerivationWorker g_keyDerivationWorker;
UINT g_msgDerivedKey = RegisterWindowMessageW(L"ClippDerivedKeyNotification");
UINT g_msgPeerDisplayUpdate = RegisterWindowMessageW(L"ClippPeerDisplayUpdate");
bool g_ignoreDerivedKeys = true;
std::array<unsigned char, KeyManager::NetworkKeySize> g_lastKnownNetworkKey;
bool g_lastKnownNetworkKeyEmpty;

std::size_t g_peerDisplayWatcherID{};
std::vector<PeerDisplayItem> g_peerDisplayItems;

constexpr std::size_t kPeerDisplayHostNameWidth = 24;
constexpr std::size_t kPeerDisplayByteCounterWidth = 15; // 12 digits plus 3 thousand separators.
constexpr uint64_t kPeerDisplayMaxByteCounter = 999'999'999'999;

std::wstring PeerDisplayHostNameField(const std::wstring& hostName) {
    std::wstring field = hostName.empty() ? L"(unknown)" : hostName;
    if (field.size() > kPeerDisplayHostNameWidth) {
        field.resize(kPeerDisplayHostNameWidth);
    } else {
        field.append(kPeerDisplayHostNameWidth - field.size(), L' ');
    }
    return field;
}

std::wstring PeerDisplayByteCounter(uint64_t bytes) {
    if (bytes > kPeerDisplayMaxByteCounter) {
        return L"+++,+++,+++,+++";
    }

    std::wstring digits = std::to_wstring(bytes);
    std::wstring counter;
    counter.reserve(kPeerDisplayByteCounterWidth);
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && ((digits.size() - i) % 3) == 0) {
            counter.push_back(L',');
        }
        counter.push_back(digits[i]);
    }

    if (counter.size() < kPeerDisplayByteCounterWidth) {
        counter.insert(counter.begin(), kPeerDisplayByteCounterWidth - counter.size(), L' ');
    }
    return counter;
}

bool PeerDisplayItemLess(const PeerDisplayItem& left, const PeerDisplayItem& right) {
    if (left.hostID != right.hostID) {
        return std::lexicographical_compare(left.hostID.begin(), left.hostID.end(), right.hostID.begin(), right.hostID.end());
    }
    return left.hostName < right.hostName;
}

std::wstring PeerDisplayItemLine(const PeerDisplayItem& item) {
    std::wstring line = PeerDisplayHostNameField(item.hostName);
    line += L" [";
    line += item.hasIncomingConnection ? L"in" : L"  ";
    line += L"/";
    line += item.hasOutgoingConnection ? L"out" : L"   ";
    line += L"] sent=";
    line += PeerDisplayByteCounter(item.bytesSent);
    line += L" recv=";
    line += PeerDisplayByteCounter(item.bytesReceived);
    return line;
}

void PeerDisplay_Render() {
    if (!g_peerDisplayTextBox) {
        return;
    }

    if (g_peerDisplayItems.empty()) {
        g_peerDisplayTextBox.Text(L"No peers connected.");
        return;
    }

    std::wstring text;
    for (const auto& item : g_peerDisplayItems) {
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += PeerDisplayItemLine(item);
    }
    g_peerDisplayTextBox.Text(text);
}

void PeerDisplay_LoadSnapshot(std::vector<PeerDisplayItem> items) {
    g_peerDisplayItems = std::move(items);
    std::sort(g_peerDisplayItems.begin(), g_peerDisplayItems.end(), PeerDisplayItemLess);
    PeerDisplay_Render();
}

void PeerDisplay_Watcher(const PeerDisplayUpdate&, void* userData) {
    const HWND hwnd = reinterpret_cast<HWND>(userData);
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, g_msgPeerDisplayUpdate, 0, 0);
    }
}

void PeerDisplay_BeginNotifications(HWND hwnd) {
    if (!hwnd || g_peerDisplayWatcherID != 0) {
        return;
    }

    const auto registration = g_peerDisplay.QueryAndRegister(PeerDisplay_Watcher, hwnd);
    g_peerDisplayWatcherID = registration.watcherID;
    PeerDisplay_LoadSnapshot(registration.items);
}

void PeerDisplay_EndNotifications() {
    if (g_peerDisplayWatcherID == 0) {
        return;
    }

    g_peerDisplay.Unregister(g_peerDisplayWatcherID);
    g_peerDisplayWatcherID = 0;
}

void PasswordFields_Setup() {
    using namespace winrt::Windows::UI::Xaml;
    std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
    bool haveNetworkKey = false;

    if (g_keyManager.GetNetworkKey(networkKey)) {
        haveNetworkKey = true;
        sodium_memzero(networkKey.data(), networkKey.size());
    }

    if (haveNetworkKey) {
        g_readOnlyText.Text(L"••••••••••••••••");
        g_passwordField.Visibility(Visibility::Collapsed);
        g_readOnlyText.Visibility(Visibility::Visible);
        g_changeBtn.Content(winrt::box_value(L"Change"));

        if (g_passwordStatusPanel) {
            g_passwordHashText.Text(g_keyManager.GetNetworkKeyHash());
            g_passwordStatusPanel.Visibility(Visibility::Visible);
            g_passwordInfoPanel.Visibility(Visibility::Collapsed);
        }
    } else {
        g_passwordField.Visibility(Visibility::Visible);
        g_readOnlyText.Visibility(Visibility::Collapsed);
        g_passwordField.Password(L"");
        g_changeBtn.Content(winrt::box_value(L"Cancel"));

        if (g_passwordStatusPanel) {
            g_passwordInfoText.Text(L"Enter network password to create or join a network.");
            g_passwordStatusPanel.Visibility(Visibility::Collapsed);
            g_passwordInfoPanel.Visibility(Visibility::Visible);
        }
    }
}

void PasswordFields_NewHashReceived() {
    using namespace winrt::Windows::UI::Xaml;
    std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
    bool haveNetworkKey = false;
    if (g_keyManager.GetNetworkKey(networkKey)) {
        sodium_memzero(networkKey.data(), networkKey.size());
        if (g_passwordStatusPanel) {
            g_passwordHashText.Text(g_keyManager.GetNetworkKeyHash());
            g_passwordStatusPanel.Visibility(Visibility::Visible);
            g_passwordInfoPanel.Visibility(Visibility::Collapsed);
        }
    }
}

void PasswordFields_BeginEdit() {
    using namespace winrt::Windows::UI::Xaml;
    g_readOnlyText.Visibility(Visibility::Collapsed);
    g_changeBtn.Content(winrt::box_value(L"Cancel"));
    g_passwordField.Password(L"••••••••••••••••");
    g_passwordField.Visibility(Visibility::Visible);
    g_passwordField.Focus(FocusState::Programmatic);
    g_passwordField.SelectAll();

    if (g_passwordStatusPanel) {
        g_passwordInfoText.Text(L"Enter network password to create or join a network.");
        g_passwordStatusPanel.Visibility(Visibility::Collapsed);
        g_passwordInfoPanel.Visibility(Visibility::Visible);
    }
}

void PasswordFields_CancelEdit() {
    g_ignoreDerivedKeys = true;
    if (!g_lastKnownNetworkKeyEmpty) {
        g_keyManager.SetNetworkKey(g_lastKnownNetworkKey);
        MDNSNotifyNetworkKeyChange();
        g_peerManager.ClearPeers();
    }
    PasswordFields_Setup();
}

void PasswordFields_ApplyEdit() {
    winrt::hstring pwd = g_passwordField.Password();

    if (pwd.size() < 8) {
        if (g_passwordInfoText) {
            g_passwordInfoText.Text(L"Password must be at least 8 characters.");
        }
        return; // Reject and wait for user to keep typing
    }

    if (g_passwordInfoText) {
        g_passwordInfoText.Text(L"... working ...");
    }

    std::string newPassword = winrt::to_string(pwd);
    g_keyDerivationWorker.RequestKeyDerivation(newPassword);
    sodium_memzero(newPassword.data(), newPassword.capacity());
    g_ignoreDerivedKeys = false;
}

void EnsureXamlInitialized() {
    static bool apartmentInitialized = false;

    if (!apartmentInitialized) {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        apartmentInitialized = true;
    }

    if (!g_xamlManager) {
        g_xamlManager = winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
    }
}

winrt::Windows::UI::Color ColorFromColorRef(COLORREF color) {
    return winrt::Windows::UI::ColorHelper::FromArgb(
        255,
        static_cast<uint8_t>(GetRValue(color)),
        static_cast<uint8_t>(GetGValue(color)),
        static_cast<uint8_t>(GetBValue(color)));
}

winrt::Windows::UI::Xaml::ElementTheme GetCurrentXamlTheme() {
    return DarkMode::isEnabled()
        ? winrt::Windows::UI::Xaml::ElementTheme::Dark
        : winrt::Windows::UI::Xaml::ElementTheme::Light;
}

winrt::Windows::UI::Xaml::Media::Brush LookupThemeBrush(const wchar_t* resourceName) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Media;

    const auto app = Application::Current();
    if (!app) {
        return nullptr;
    }

    const auto resources = app.Resources();
    const auto key = winrt::box_value(winrt::hstring{ resourceName });
    if (!resources.HasKey(key)) {
        return nullptr;
    }

    return resources.Lookup(key).as<Brush>();
}

winrt::Windows::UI::Xaml::Media::Brush GetThemeBackgroundBrush() {
    using namespace winrt::Windows::UI::Xaml::Media;

    if (DarkMode::isEnabled()) {
        if (const auto chromeBrush = LookupThemeBrush(L"SystemControlBackgroundChromeMediumLowBrush")) {
            return chromeBrush;
        }
    }

    if (const auto pageBrush = LookupThemeBrush(L"ApplicationPageBackgroundThemeBrush")) {
        return pageBrush;
    }

    const COLORREF fallbackColor = DarkMode::isEnabled()
        ? DarkMode::getDlgBackgroundColor()
        : GetSysColor(COLOR_3DFACE);
    return SolidColorBrush(ColorFromColorRef(fallbackColor));
}

winrt::Windows::UI::Xaml::Controls::ScrollViewer CreateTerminalLikeScrollViewer() {
    auto terminalLogView = std::make_unique<TerminalLogView>();
    auto view = terminalLogView->View();

    {
        std::lock_guard<std::mutex> lock(g_terminalLogViewMutex);
        g_terminalLogView = std::move(terminalLogView);
    }

    return view;
}

winrt::Windows::UI::Xaml::Controls::StackPanel CreateNetworkCard() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Controls::Primitives;
    using namespace winrt::Windows::UI::Xaml::Media;
    using namespace winrt::Windows::UI::Text;
    // 1. The Outer Card Container
    StackPanel card;
    // Optional: Give the card a subtle background to match Windows 11 settings
    // card.Background(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(10, 255, 255, 255)));
    card.CornerRadius(CornerRadius{ 4 });
    card.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    // Use a subtle border color
    card.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));

    // 2. The Header Row

    // Helper to cleanly create columns
    auto makeCol = [](double value, GridUnitType type) {
        ColumnDefinition col;
        col.Width(GridLength{ value, type });
        return col;
        };

    Grid headerGrid;
    headerGrid.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    headerGrid.ColumnDefinitions().Append(makeCol(40, GridUnitType::Pixel)); // Main Icon
    headerGrid.ColumnDefinitions().Append(makeCol(1, GridUnitType::Star));  // Text
    headerGrid.ColumnDefinitions().Append(makeCol(45, GridUnitType::Pixel)); // IN/OUT ICONS
    headerGrid.ColumnDefinitions().Append(makeCol(40, GridUnitType::Pixel)); // Chevron

    // Network Icon (Using standard Windows 10/11 Segoe MDL2 Assets)
    FontIcon netIcon;
    netIcon.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
    netIcon.Glyph(L"\xE839"); // E839 is a common network/Ethernet glyph
    netIcon.FontSize(18);
    netIcon.VerticalAlignment(VerticalAlignment::Center);
    netIcon.HorizontalAlignment(HorizontalAlignment::Left);
    Grid::SetColumn(netIcon, 0);

    // Text Stack (Name and Details)
    StackPanel textStack;
    textStack.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(textStack, 1);

    TextBlock title;
    title.Text(L"MacMini.local");
    title.FontSize(14);
    // "Brighter" text by using SemiBold
    title.FontWeight(FontWeights::SemiBold());

    TextBlock subtitle;
    subtitle.Text(L"Connected for 5 days, 14:23:43");
    subtitle.FontSize(12);
    // "Duller" text by dropping opacity
    subtitle.Opacity(0.6);

    textStack.Children().Append(title);
    textStack.Children().Append(subtitle);

    // Chevron Toggle Button
    ToggleButton chevron;
    chevron.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
    chevron.Content(winrt::box_value(L"\xE70D")); // ChevronDown
    chevron.Background(SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
    chevron.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 0));
    chevron.VerticalAlignment(VerticalAlignment::Center);
    chevron.HorizontalAlignment(HorizontalAlignment::Right);
    Grid::SetColumn(chevron, 3);

    StackPanel statusIcons;
    statusIcons.Orientation(Orientation::Horizontal);
    statusIcons.VerticalAlignment(VerticalAlignment::Center);
    statusIcons.HorizontalAlignment(HorizontalAlignment::Right);
    Grid::SetColumn(statusIcons, 2);

    FontIcon inIcon;
    inIcon.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
    inIcon.Glyph(L"\xE118"); // Download/Incoming arrow
    inIcon.FontSize(12);
    inIcon.Width(20); // Fixed width so layout doesn't jump
    inIcon.Foreground(SolidColorBrush(winrt::Windows::UI::Colors::LimeGreen()));
    // inIcon.Visibility(Visibility::Collapsed); // Toggle this based on state

    FontIcon outIcon;
    outIcon.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
    outIcon.Glyph(L"\xE11C"); // Upload/Outgoing arrow
    outIcon.FontSize(12);
    outIcon.Width(20);
    outIcon.Foreground(SolidColorBrush(winrt::Windows::UI::Colors::DeepSkyBlue()));
    // outIcon.Visibility(Visibility::Collapsed); // Toggle this based on state

    statusIcons.Children().Append(inIcon);
    statusIcons.Children().Append(outIcon);

    headerGrid.Children().Append(netIcon);
    headerGrid.Children().Append(textStack);
    headerGrid.Children().Append(chevron);
    headerGrid.Children().Append(statusIcons);

    // 3. The Collapsible Content (Two-Column Layout)
    Grid contentGrid;
    contentGrid.Visibility(Visibility::Collapsed);
    // Nice margins: pad the left to align with the text above, pad the bottom
    contentGrid.Padding(ThicknessHelper::FromLengths(56, 0, 16, 16));
    contentGrid.ColumnDefinitions().Append(makeCol(130, GridUnitType::Pixel)); // Headers
    contentGrid.ColumnDefinitions().Append(makeCol(1, GridUnitType::Star));  // Values    Grid headerGrid;

    // Helper lambda to easily add rows to our table
    auto addRow = [&](int rowIndex, const wchar_t* labelText, const wchar_t* valueText) {
        RowDefinition rowDef;
        rowDef.Height(GridLength{ 1, GridUnitType::Auto });
        contentGrid.RowDefinitions().Append(rowDef);

        TextBlock label;
        label.Text(labelText);
        label.FontSize(13);
        label.FontWeight(FontWeights::SemiBold()); // Brighter/heavier text for the left side
        label.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
        Grid::SetColumn(label, 0);
        Grid::SetRow(label, rowIndex);

        TextBlock value;
        value.Text(valueText);
        value.FontSize(13);
        value.Opacity(0.7); // Duller text for the right side
        value.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
        Grid::SetColumn(value, 1);
        Grid::SetRow(value, rowIndex);

        contentGrid.Children().Append(label);
        contentGrid.Children().Append(value);
    };

    // Populate the table
    addRow(1, L"Bytes sent:", L"5,339,546,273");
    addRow(2, L"Bytes received:", L"2,946,613,942");
    addRow(3, L"Incoming connection:", L"Ok");
    addRow(4, L"Outgoing connection:", L"Ok");
    addRow(5, L"", L"");

    // 4. The Interaction Logic
    // When the chevron is clicked, toggle the content visibility and flip the arrow
    chevron.Click([=](winrt::Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
        auto toggle = sender.as<ToggleButton>();
        if (toggle.IsChecked().GetBoolean()) {
            contentGrid.Visibility(Visibility::Visible);
            toggle.Content(winrt::box_value(L"\xE70E")); // ChevronUp
        }
        else {
            contentGrid.Visibility(Visibility::Collapsed);
            toggle.Content(winrt::box_value(L"\xE70D")); // ChevronDown
        }
        });

    // Assemble the final card
    card.Children().Append(headerGrid);
    card.Children().Append(contentGrid);

    return card;
}

winrt::Windows::UI::Xaml::Controls::Grid BuildPlaceholderContent() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Media;

    Grid root;
    root.RequestedTheme(GetCurrentXamlTheme());
    root.Background(GetThemeBackgroundBrush());

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
    intro.Text(L"Secure cross-platform clipboard sync with peer-to-peer networking.");
    intro.FontSize(14);
    intro.TextWrapping(TextWrapping::WrapWholeWords);
    content.Children().Append(intro);


    // --- 1. Password Header ---
    TextBlock passwordHeader;
    passwordHeader.Text(L"Network");
    passwordHeader.FontSize(16);
    passwordHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());

    TextBlock passwordLabel;
    passwordLabel.Text(L"Network secret");
    passwordLabel.VerticalAlignment(VerticalAlignment::Center);

    g_readOnlyText = TextBlock();
    g_readOnlyText.VerticalAlignment(VerticalAlignment::Center);

    // Editable Input
    winrt::Windows::UI::Xaml::DispatcherTimer debounceTimer;
    debounceTimer.Interval(std::chrono::milliseconds(500));
    debounceTimer.Tick([=](winrt::Windows::Foundation::IInspectable const&, winrt::Windows::Foundation::IInspectable const&) {
        debounceTimer.Stop();
        PasswordFields_ApplyEdit();
        });

    g_passwordField = PasswordBox();
    g_passwordField.VerticalAlignment(VerticalAlignment::Center);
    g_passwordField.Visibility(Visibility::Collapsed);
    g_passwordField.MinWidth(200);
    g_passwordField.KeyDown([](auto&& sender, winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
        if (e.Key() == winrt::Windows::System::VirtualKey::Escape) {
            PasswordFields_CancelEdit();
            e.Handled(true);
        }
        });
    g_passwordField.PasswordChanged([=](winrt::Windows::Foundation::IInspectable const&, winrt::Windows::UI::Xaml::RoutedEventArgs const&) {
        debounceTimer.Stop();
        debounceTimer.Start();
        });

    // The Action Button
    g_changeBtn = Button();
    g_changeBtn.Content(winrt::box_value(L"Change"));
    g_changeBtn.VerticalAlignment(VerticalAlignment::Center);
    g_changeBtn.HorizontalAlignment(HorizontalAlignment::Right); // Right aligned!

    g_changeBtn.Click([=](winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
        if (g_passwordField.Visibility() == Visibility::Visible) {
            PasswordFields_CancelEdit();
        }
        else {
            PasswordFields_BeginEdit();
        }
        });

    // --- 2. Input Row Grid ---
    Grid inputGrid;
    inputGrid.CornerRadius(CornerRadius{ 4 });
    inputGrid.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    inputGrid.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));
    inputGrid.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    ColumnDefinition col1, col2;
    col1.Width(GridLength{ 1, GridUnitType::Star });
    col2.Width(GridLength{ 1, GridUnitType::Auto });
    inputGrid.ColumnDefinitions().Append(col1);
    inputGrid.ColumnDefinitions().Append(col2);

    StackPanel inputLeft;
    inputLeft.Orientation(Orientation::Horizontal);
    inputLeft.Spacing(10);
    inputLeft.VerticalAlignment(VerticalAlignment::Center);
    inputLeft.Children().Append(passwordLabel);
    inputLeft.Children().Append(g_readOnlyText);
    inputLeft.Children().Append(g_passwordField);

    Grid::SetColumn(inputLeft, 0);
    Grid::SetColumn(g_changeBtn, 1);
    inputGrid.Children().Append(inputLeft);
    inputGrid.Children().Append(g_changeBtn);

    // --- 3. Status Panel (Key Icon, Bold Hash, Explainer) ---
    g_passwordStatusPanel = StackPanel();
    g_passwordStatusPanel.CornerRadius(CornerRadius{ 4 });
    g_passwordStatusPanel.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    g_passwordStatusPanel.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));
    g_passwordStatusPanel.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    g_passwordStatusPanel.Orientation(Orientation::Horizontal);
    g_passwordStatusPanel.Spacing(12);

    FontIcon keyIcon;
    keyIcon.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
    keyIcon.Glyph(L"\xE8D7"); // Key icon
    keyIcon.FontSize(18);
    keyIcon.VerticalAlignment(VerticalAlignment::Center);

    StackPanel statusTextStack;
    statusTextStack.Orientation(Orientation::Vertical);
    statusTextStack.Spacing(2);
    statusTextStack.VerticalAlignment(VerticalAlignment::Center);

    g_passwordHashText = TextBlock();
    g_passwordHashText.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
    g_passwordHashText.TextWrapping(TextWrapping::Wrap);

    TextBlock hashExplainer;
    hashExplainer.Text(L"Not your actual network key; this is the SHA256 of Argon2id. Used only on this screen.");
    hashExplainer.Opacity(0.6); // Muted/darker text style
    hashExplainer.TextWrapping(TextWrapping::Wrap);

    statusTextStack.Children().Append(g_passwordHashText);
    statusTextStack.Children().Append(hashExplainer);

    g_passwordStatusPanel.Children().Append(keyIcon);
    g_passwordStatusPanel.Children().Append(statusTextStack);

    // --- 4. Info Panel (Info Icon, Notification Text) ---
    g_passwordInfoPanel = StackPanel();
    g_passwordInfoPanel.CornerRadius(CornerRadius{ 4 });
    g_passwordInfoPanel.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    g_passwordInfoPanel.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));
    g_passwordInfoPanel.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    g_passwordInfoPanel.Orientation(Orientation::Horizontal);
    g_passwordInfoPanel.Spacing(12);

    FontIcon infoIcon;
    infoIcon.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
    infoIcon.Glyph(L"\xE946"); // Info icon
    infoIcon.FontSize(18);
    infoIcon.VerticalAlignment(VerticalAlignment::Center);

    g_passwordInfoText = TextBlock();
    g_passwordInfoText.VerticalAlignment(VerticalAlignment::Center);

    g_passwordInfoPanel.Children().Append(infoIcon);
    g_passwordInfoPanel.Children().Append(g_passwordInfoText);

    // --- Container Assembly ---
    StackPanel outerContainer;
    outerContainer.Orientation(Orientation::Vertical);
    outerContainer.Spacing(10);

    outerContainer.Children().Append(passwordHeader);
    outerContainer.Children().Append(inputGrid);
    outerContainer.Children().Append(g_passwordStatusPanel);
    outerContainer.Children().Append(g_passwordInfoPanel);

    TextBlock peerDisplayLabel;
    peerDisplayLabel.Text(L"Peers");
    peerDisplayLabel.FontSize(16);
    peerDisplayLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    outerContainer.Children().Append(peerDisplayLabel);

    g_peerDisplayTextBox = TextBox();
    g_peerDisplayTextBox.AcceptsReturn(true);
    g_peerDisplayTextBox.IsReadOnly(true);
    g_peerDisplayTextBox.MinHeight(120);
    g_peerDisplayTextBox.TextWrapping(TextWrapping::NoWrap);
    g_peerDisplayTextBox.FontFamily(FontFamily(L"Consolas"));
    g_peerDisplayTextBox.Text(L"No peers connected.");
    outerContainer.Children().Append(g_peerDisplayTextBox);

    content.Children().Append(outerContainer);
    content.Children().Append(CreateNetworkCard());
    content.Children().Append(CreateTerminalLikeScrollViewer());

    /*
    TextBlock listHeading;
    listHeading.Text(L"Placeholder activity");
    listHeading.FontSize(16);
    listHeading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    content.Children().Append(listHeading);

    ListView listView;
    listView.MinHeight(140);
    listView.MaxHeight(220);
    listView.Items().Append(winrt::box_value(L"Clipboard sync status placeholder"));
    listView.Items().Append(winrt::box_value(L"Known peers placeholder"));
    listView.Items().Append(winrt::box_value(L"Recent clipboard item placeholder"));
    content.Children().Append(listView);

    StackPanel actions;
    actions.Orientation(Orientation::Horizontal);
    actions.Spacing(8);

    Button primaryButton;
    primaryButton.Content(winrt::box_value(L"Primary action"));
    actions.Children().Append(primaryButton);

    Button secondaryButton;
    secondaryButton.Content(winrt::box_value(L"Secondary action"));
    actions.Children().Append(secondaryButton);

    ToggleSwitch toggle;
    toggle.Header(winrt::box_value(L"Sample toggle"));
    toggle.IsOn(true);
    actions.Children().Append(toggle);

    content.Children().Append(actions);
    root.Children().Append(content);
    */
    winrt::Windows::UI::Xaml::Controls::ScrollViewer mainScroll;
    mainScroll.VerticalScrollBarVisibility(winrt::Windows::UI::Xaml::Controls::ScrollBarVisibility::Auto);
    mainScroll.Content(content); // Put the whole StackPanel inside the ScrollViewer
    root.Children().Append(mainScroll);

    g_uiDispatcher = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();

    return root;
}

void ResizeXamlHost(HWND hwnd);

void ApplyModernWindowAttributes(HWND hwnd) {
    // Prefer the newer dark-title-bar attribute, but fall back to the earlier Windows 10 value.
    BOOL useDarkTitleBar = DarkMode::isEnabled() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkTitleBar, sizeof(useDarkTitleBar));
    DwmSetWindowAttribute(hwnd, 19 /* DWMWA_USE_IMMERSIVE_DARK_MODE before Windows 10 20H1 */, &useDarkTitleBar, sizeof(useDarkTitleBar));

    const DWORD cornerPreferenceRound = 2; // DWMWCP_ROUND on Windows 11; ignored on older builds.
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPreferenceRound, sizeof(cornerPreferenceRound));
}

int DipsToPixels(double dips, UINT dpi) {
    return static_cast<int>(std::ceil(dips * dpi / USER_DEFAULT_SCREEN_DPI));
}

RECT WindowRectForClientSize(HWND hwnd, int clientWidth, int clientHeight) {
    RECT rect{ 0, 0, clientWidth, clientHeight };
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    const BOOL hasMenu = GetMenu(hwnd) != nullptr;
    const UINT dpi = GetDpiForWindow(hwnd);

    if (!AdjustWindowRectExForDpi(&rect, style, hasMenu, exStyle, dpi)) {
        AdjustWindowRectEx(&rect, style, hasMenu, exStyle);
    }

    return rect;
}

SIZE DesiredClientSizePixels(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    double widthDips = kDialogDefaultClientWidthDips;
    double heightDips = kDialogDefaultClientHeightDips;

    auto* state = reinterpret_cast<XamlDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state && state->xamlSource) {
        const auto content = state->xamlSource.Content();
        if (content) {
            content.Measure(winrt::Windows::Foundation::Size{
                static_cast<float>(kDialogDefaultClientWidthDips),
                std::numeric_limits<float>::infinity()
            });

            const auto desired = content.DesiredSize();
            widthDips = (std::max)(widthDips, static_cast<double>(desired.Width));
            heightDips = (std::max)(heightDips, static_cast<double>(desired.Height));
        }
    }

    widthDips = (std::max)(widthDips, kDialogMinClientWidthDips);
    heightDips = (std::max)(heightDips, kDialogMinClientHeightDips);

    return SIZE{ DipsToPixels(widthDips, dpi), DipsToPixels(heightDips, dpi) };
}

HMONITOR DialogMonitor(HWND hwnd, HWND owner) {
    if (owner) {
        if (HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONULL)) {
            return monitor;
        }
    }

    if (HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL)) {
        return monitor;
    }

    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        return MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    }

    return MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
}

void SizeAndCenterDialog(HWND hwnd, HWND owner) {
    const SIZE desiredClientSize = DesiredClientSizePixels(hwnd);
    RECT windowRect = WindowRectForClientSize(hwnd, desiredClientSize.cx, desiredClientSize.cy);
    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    GetMonitorInfoW(DialogMonitor(hwnd, owner), &monitorInfo);

    const RECT workArea = monitorInfo.rcWork;
    const int workWidth = workArea.right - workArea.left;
    const int workHeight = workArea.bottom - workArea.top;
    const UINT dpi = GetDpiForWindow(hwnd);
    const int margin = DipsToPixels(24, dpi);

    windowWidth = (std::min)(windowWidth, (std::max)(1, workWidth - (margin * 2)));
    windowHeight = (std::min)(windowHeight, (std::max)(1, workHeight - (margin * 2)));

    const int x = workArea.left + ((workWidth - windowWidth) / 2);
    const int y = workArea.top + ((workHeight - windowHeight) / 2);

    SetWindowPos(hwnd, nullptr, x, y, windowWidth, windowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    ResizeXamlHost(hwnd);
}

void ResizeXamlHost(HWND hwnd) {
    auto* state = reinterpret_cast<XamlDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!state || !state->xamlHost) {
        return;
    }

    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    SetWindowPos(
        state->xamlHost,
        nullptr,
        0,
        0,
        clientRect.right - clientRect.left,
        clientRect.bottom - clientRect.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(state->xamlHost, SW_SHOW);
}

void InitializeXamlIsland(HWND hwnd) {
    EnsureXamlInitialized();

    auto state = std::make_unique<XamlDialogState>();
    state->xamlSource = winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource{};

    auto nativeSource = state->xamlSource.as<IDesktopWindowXamlSourceNative>();
    winrt::check_hresult(nativeSource->AttachToWindow(hwnd));
    winrt::check_hresult(nativeSource->get_WindowHandle(&state->xamlHost));

    state->xamlSource.Content(BuildPlaceholderContent());
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
    state.release();
    ResizeXamlHost(hwnd);
}

void LogReflectorCallback(const std::wstring& line) {
    if (!g_uiDispatcher) {
        return;
    }

    g_uiDispatcher.TryEnqueue([line]() {
        std::lock_guard<std::mutex> lock(g_terminalLogViewMutex);
        if (g_terminalLogView) {
            g_terminalLogView->AppendAnsiLogText(line);
        }
    });
}

LRESULT CALLBACK MainDialogWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        try {
            DarkMode::setWindowEraseBgSubclass(hwnd);
            DarkMode::setDarkWndNotifySafe(hwnd, true);
            ApplyModernWindowAttributes(hwnd);
            InitializeXamlIsland(hwnd);
			g_keyDerivationWorker.SetNotificationTarget(hwnd, g_msgDerivedKey);
        }
        catch (const winrt::hresult_error& error) {
            g_createError = L"Unable to open the XAML Islands dialog. This requires Windows 10 version 1903 or later and the WinRT XAML hosting runtime.\n\nHRESULT: " +
                std::to_wstring(static_cast<uint32_t>(static_cast<int32_t>(error.code())));
            g_logger.log(__FUNCTION__, Logger::Level::Error, g_createError.c_str());
            return -1;
        }
        return 0;

    case WM_SHOWWINDOW:
        if (wParam) {
            PeerDisplay_BeginNotifications(hwnd);
            g_logger.AddLogReflector(LogReflectorCallback);
        } else {
            PeerDisplay_EndNotifications();
            g_logger.RemoveLogReflector(LogReflectorCallback);
        }
        return 0;

    case WM_SIZE:
        ResizeXamlHost(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(hwnd);
        const RECT minWindowRect = WindowRectForClientSize(
            hwnd,
            DipsToPixels(kDialogMinClientWidthDips, dpi),
            DipsToPixels(kDialogMinClientHeightDips, dpi));
        minMaxInfo->ptMinTrackSize.x = minWindowRect.right - minWindowRect.left;
        minMaxInfo->ptMinTrackSize.y = minWindowRect.bottom - minWindowRect.top;
        return 0;
    }

    case WM_ACTIVATE:
        if (wParam != WA_INACTIVE) {
            SetForegroundWindow(hwnd);
            if (g_uiDispatcher) {
                g_uiDispatcher.TryEnqueue([]() {
                    PasswordFields_Setup();
                });
            }
            g_lastKnownNetworkKeyEmpty = !g_keyManager.GetNetworkKey(g_lastKnownNetworkKey);
        }
		return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY: {
        PeerDisplay_EndNotifications();
        g_logger.RemoveLogReflector(LogReflectorCallback);
        {
            std::lock_guard<std::mutex> lock(g_terminalLogViewMutex);
            g_terminalLogView.reset();
        }

        auto* state = reinterpret_cast<XamlDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete state;

        if (g_dialogWindow == hwnd) {
            g_dialogWindow = nullptr;
        }
        return 0;
    }

    default:
        if (msg == g_msgDerivedKey) {
            if (!g_ignoreDerivedKeys) {
                KeyDerivationWorker::KeyDerivationResult* result = reinterpret_cast<KeyDerivationWorker::KeyDerivationResult*>(wParam);

                g_keyManager.SetNetworkKey(result->derivedKey);
                MDNSNotifyNetworkKeyChange();
                g_peerManager.ClearPeers();

                // Refresh the UI safely to show the status panel and hide the info panel
                if (g_uiDispatcher) {
                    g_uiDispatcher.TryEnqueue([]() {
                        PasswordFields_NewHashReceived();
                        });
                }
            }
            return 0;
		}

        if (msg == g_msgPeerDisplayUpdate) {
            if (g_peerDisplayWatcherID != 0) {
                PeerDisplay_LoadSnapshot(g_peerDisplay.Query());
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void RegisterDialogClass(HINSTANCE hInstance) {
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainDialogWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kDialogClassName;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    RegisterClassW(&wc);
    registered = true;
}

} // namespace

void ShowClippMainDialog(HWND owner) {
    try {
        HINSTANCE hInstance = GetModuleHandleW(nullptr);
        RegisterDialogClass(hInstance);

        if (!g_dialogWindow) {
            g_createError.clear();
            g_dialogWindow = CreateWindowExW(
                WS_EX_APPWINDOW,
                kDialogClassName,
                L"Clipp",
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                720,
                520,
                nullptr,
                nullptr,
                hInstance,
                nullptr);
        }

        if (g_dialogWindow) {
            SizeAndCenterDialog(g_dialogWindow, owner);
            ShowWindow(g_dialogWindow, SW_SHOWNORMAL);
            SetForegroundWindow(g_dialogWindow);
        }
        else if (!g_createError.empty()) {
            MessageBoxW(owner, g_createError.c_str(), L"Clipp", MB_ICONERROR | MB_OK);
        }
    }
    catch (const winrt::hresult_error& error) {
        const std::wstring message = L"Unable to open the XAML Islands dialog. This requires Windows 10 version 1903 or later and the WinRT XAML hosting runtime.\n\nHRESULT: " + std::to_wstring(static_cast<uint32_t>(static_cast<int32_t>(error.code())));
        g_logger.log(__FUNCTION__, Logger::Level::Error, message.c_str());
        MessageBoxW(owner, message.c_str(), L"Clipp", MB_ICONERROR | MB_OK);
    }
}

bool ClippMainDialogPreTranslateMessage(MSG* msg) {
    if (!g_dialogWindow || !msg) {
        return false;
    }

    auto* state = reinterpret_cast<XamlDialogState*>(GetWindowLongPtrW(g_dialogWindow, GWLP_USERDATA));
    if (!state || !state->xamlSource) {
        return false;
    }

    auto nativeSource = state->xamlSource.as<IDesktopWindowXamlSourceNative2>();
    BOOL handled = FALSE;
    return SUCCEEDED(nativeSource->PreTranslateMessage(msg, &handled)) && handled;
}

void CloseClippMainDialog() {
    if (g_dialogWindow) {
        DestroyWindow(g_dialogWindow);
        g_dialogWindow = nullptr;
    }

    if (g_xamlManager) {
        g_xamlManager.Close();
        g_xamlManager = nullptr;
    }
}
