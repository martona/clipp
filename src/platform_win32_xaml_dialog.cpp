#include "PeerManager.h"
#include "PeerDisplay.h"
#include "platform_win32_xaml_dialog.h"
#include "platform_win32_NetworkView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

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

winrt::Windows::UI::Xaml::Controls::TextBox g_networkNameField{ nullptr };
winrt::Windows::UI::Xaml::Controls::PasswordBox g_passwordField{ nullptr };
winrt::Windows::System::DispatcherQueue g_uiDispatcher{ nullptr };
std::unique_ptr<TerminalLogView> g_terminalLogView;
std::unique_ptr<NetworkView> g_networkView;
winrt::Windows::UI::Xaml::DispatcherTimer g_networkPollTimer{ nullptr };
std::mutex g_terminalLogViewMutex;
winrt::Windows::UI::Xaml::Controls::StackPanel g_passwordStatusPanel{ nullptr };
winrt::Windows::UI::Xaml::Controls::TextBlock g_passwordHashText{ nullptr };
winrt::Windows::UI::Xaml::Controls::StackPanel g_passwordInfoPanel{ nullptr };
winrt::Windows::UI::Xaml::Controls::TextBlock g_passwordInfoText{ nullptr };

KeyDerivationWorker g_keyDerivationWorker;
UINT g_msgDerivedKey = RegisterWindowMessageW(L"ClippDerivedKeyNotification");
UINT g_msgPeerDisplayUpdate = RegisterWindowMessageW(L"ClippPeerDisplayUpdate");

std::size_t g_peerDisplayWatcherID{};

void NetworkView_Poll() {
    if (g_networkView) {
        g_networkView->Poll();
    }
}

void NetworkView_StartPollTimer() {
    if (!g_networkPollTimer) {
        g_networkPollTimer = winrt::Windows::UI::Xaml::DispatcherTimer();
        g_networkPollTimer.Interval(std::chrono::seconds(1));
        g_networkPollTimer.Tick([](auto const&, auto const&) {
            NetworkView_Poll();
        });
    }
    g_networkPollTimer.Start();
}

void NetworkView_StopPollTimer() {
    if (g_networkPollTimer) {
        g_networkPollTimer.Stop();
    }
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
    NetworkView_Poll();
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

    if (g_keyManager.HaveNetworkKey()) {
        g_passwordField.Password(L"••••••••••••••••");
        g_passwordHashText.Text(g_keyManager.GetNetworkKeyHash());
        g_passwordStatusPanel.Visibility(Visibility::Visible);
        g_passwordInfoPanel.Visibility(Visibility::Collapsed);
    } else {
        g_passwordField.Password(L"");
        g_passwordInfoText.Text(L"Enter network secret to create or join a network.");
        g_passwordStatusPanel.Visibility(Visibility::Collapsed);
        g_passwordInfoPanel.Visibility(Visibility::Visible);
    }
}

void PasswordFields_NewHashReceived() {
    using namespace winrt::Windows::UI::Xaml;
    if (g_keyManager.HaveNetworkKey()) {
        g_passwordHashText.Text(g_keyManager.GetNetworkKeyHash());
        g_passwordStatusPanel.Visibility(Visibility::Visible);
        g_passwordInfoPanel.Visibility(Visibility::Collapsed);
    }
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

    TextBlock networkNameLabel;
    networkNameLabel.Text(L"Name");
    networkNameLabel.VerticalAlignment(VerticalAlignment::Center);

    TextBlock passwordLabel;
    passwordLabel.Text(L"Secret");
    passwordLabel.VerticalAlignment(VerticalAlignment::Center);

    g_networkNameField = TextBox();
    g_networkNameField.VerticalAlignment(VerticalAlignment::Center);

    std::string currentName = g_settings.networkName();
    size_t size_needed = utf8_to_utf16(currentName.c_str(), currentName.length(), nullptr, 0);
    std::wstring wCurrentName(size_needed, 0);
    utf8_to_utf16(currentName.c_str(), currentName.length(), &wCurrentName[0], size_needed);
    g_networkNameField.Text(wCurrentName);

    g_networkNameField.LostFocus([](auto const& sender, auto const&) {
            auto tb = sender.as<winrt::Windows::UI::Xaml::Controls::TextBox>();
            std::string newName = winrt::to_string(tb.Text());

            if (newName != g_settings.networkName() && !newName.empty()) {
                g_settings.set_networkName(newName);
                auto now = std::chrono::system_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
                g_settings.set_networkNameTimestamp(static_cast<uint64_t>(duration.count()));
				g_keyManager.ClearNetworkKey(); // Clear the key to force re-derivation with the new name
                MDNSNotifyNetworkKeyChange();
                g_peerManager.ClearPeers();
				PasswordFields_Setup();
            }
        });

    // Editable Input
    winrt::Windows::UI::Xaml::DispatcherTimer debounceTimer;
    debounceTimer.Interval(std::chrono::milliseconds(500));
    debounceTimer.Stop();
    debounceTimer.Tick([=](auto const&, auto const&) {
            debounceTimer.Stop();
            winrt::hstring pwd = g_passwordField.Password();

            g_passwordStatusPanel.Visibility(Visibility::Collapsed);
            g_passwordInfoPanel.Visibility(Visibility::Visible);

            if (pwd.size() < 8) {
                g_passwordInfoText.Text(L"Password must be at least 8 characters.");
                return; // Reject and wait for user to keep typing
            }

            g_passwordInfoText.Text(L"... working ...");

            std::string newPassword = winrt::to_string(pwd);
            std::string netNameAndPassword = g_settings.networkName() + "|";
            netNameAndPassword += newPassword;
            g_keyDerivationWorker.RequestKeyDerivation(netNameAndPassword);
            sodium_memzero(newPassword.data(), newPassword.capacity());
            sodium_memzero(netNameAndPassword.data(), netNameAndPassword.capacity());
        });

    g_passwordField = PasswordBox();
    g_passwordField.VerticalAlignment(VerticalAlignment::Center);
    g_passwordField.MinWidth(200);
    g_passwordField.Tag(winrt::box_value(false));
    g_passwordField.GotFocus([](auto const& sender, auto const&) {
            g_passwordField.Password(L"");
        });
    g_passwordField.LostFocus([](auto const& sender, auto const&) {
            PasswordFields_Setup();
        });
    g_passwordField.KeyDown([](auto const& sender, auto const& e) {
            //if (e.Key() == winrt::Windows::System::VirtualKey::Escape) {
            //    PasswordFields_CancelEdit();
            //    e.Handled(true);
            //}
        });
    g_passwordField.PasswordChanged([=](auto const& sender, auto const&) {
            debounceTimer.Stop();
            if (g_passwordField.Password() != L"" &&
                g_passwordField.Password() != L"••••••••••••••••") {
                debounceTimer.Start();
            }
        });

    // --- 2. Input Row Grid ---
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

    // Row 0: Network Name
    Grid::SetRow(networkNameLabel, 0);
    Grid::SetColumn(networkNameLabel, 0);
    Grid::SetRow(g_networkNameField, 0);
    Grid::SetColumn(g_networkNameField, 1);

    // Row 1: Network Secret
    Grid::SetRow(passwordLabel, 1);
    Grid::SetColumn(passwordLabel, 0);
    Grid::SetRow(g_passwordField, 1);
    Grid::SetColumn(g_passwordField, 1);

    inputGrid.Children().Append(networkNameLabel);
    inputGrid.Children().Append(g_networkNameField);
    inputGrid.Children().Append(passwordLabel);
    inputGrid.Children().Append(g_passwordField);

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
    hashExplainer.Text(L"Network key fingerprint. Used only on this screen; not in itself a secret.");
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

    content.Children().Append(outerContainer);

    g_networkView = std::make_unique<NetworkView>(g_peerDisplay);
    content.Children().Append(g_networkView->View());
    NetworkView_Poll();

    content.Children().Append(CreateTerminalLikeScrollViewer());

    winrt::Windows::UI::Xaml::Controls::ScrollViewer mainScroll;
    mainScroll.VerticalScrollBarVisibility(winrt::Windows::UI::Xaml::Controls::ScrollBarVisibility::Auto);
    mainScroll.Content(content); 
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
            NetworkView_StartPollTimer();
            g_logger.AddLogReflector(LogReflectorCallback);
            if (g_uiDispatcher) {
                g_uiDispatcher.TryEnqueue([]() {
                        PasswordFields_Setup();
                    });
            }
        } else {
            PeerDisplay_EndNotifications();
            NetworkView_StopPollTimer();
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

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY: {
        PeerDisplay_EndNotifications();
        NetworkView_StopPollTimer();
        g_logger.RemoveLogReflector(LogReflectorCallback);
        {
            std::lock_guard<std::mutex> lock(g_terminalLogViewMutex);
            g_terminalLogView.reset();
        }
        g_networkView.reset();
        g_networkPollTimer = nullptr;

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
            return 0;
		}

        if (msg == g_msgPeerDisplayUpdate) {
            if (g_peerDisplayWatcherID != 0) {
                NetworkView_Poll();
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
