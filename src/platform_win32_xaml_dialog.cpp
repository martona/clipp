#include "platform_win32_xaml_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
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
#include <winrt/base.h>

#include "Logger.h"
#include "clipp-win32-darkmode32/DMSubclass.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "windowsapp.lib")

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
    heading.Text(L"Clipp main dialog");
    heading.FontSize(28);
    heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    heading.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(heading);

    TextBlock intro;
    intro.Text(L"This is placeholder XAML Islands content for the future Clipp main dialog. The controls below are boilerplate so we can validate tray integration, focus, resizing, and deployment behavior.");
    intro.FontSize(14);
    intro.TextWrapping(TextWrapping::WrapWholeWords);
    content.Children().Append(intro);

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

    // 1. Create the container
    StackPanel container;
    container.Orientation(Orientation::Horizontal);
    container.Spacing(10);

    // 2. The Read-Only Text
    TextBlock readOnlyText;
    readOnlyText.Text(L"Current Value: 15353");
    readOnlyText.VerticalAlignment(VerticalAlignment::Center);
    readOnlyText.Visibility(Visibility::Visible);

    // 3. The Editable Input (Hidden by default)
    TextBox editBox;
    editBox.Text(L"15353");
    editBox.VerticalAlignment(VerticalAlignment::Center);
    editBox.Visibility(Visibility::Collapsed);
    // Optional: Make it look a bit cleaner inline
    editBox.MinWidth(100);

    // 4. The Action Button
    Button changeBtn;
    changeBtn.Content(winrt::box_value(L"Change"));
    changeBtn.VerticalAlignment(VerticalAlignment::Center);

    // 5. The Event Handler (The "Morph" logic)
    changeBtn.Click([=](winrt::Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
        // If we are in "Read" mode, switch to "Edit" mode
        if (readOnlyText.Visibility() == Visibility::Visible) {
            readOnlyText.Visibility(Visibility::Collapsed);
            editBox.Visibility(Visibility::Visible);

            changeBtn.Content(winrt::box_value(L"Save"));

            // Optional UX polish: auto-focus the textbox and select the text
            editBox.Focus(FocusState::Programmatic);
            editBox.SelectAll();
        }
        // If we are in "Edit" mode, save and switch back to "Read" mode
        else {
            // ... Save your value here ...
            readOnlyText.Text(L"Current Value: " + editBox.Text());

            editBox.Visibility(Visibility::Collapsed);
            readOnlyText.Visibility(Visibility::Visible);

            changeBtn.Content(winrt::box_value(L"Change"));
        }
        });

    container.Children().Append(readOnlyText);
    container.Children().Append(editBox);
    container.Children().Append(changeBtn);
	content.Children().Append(container);

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

LRESULT CALLBACK MainDialogWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        try {
            DarkMode::setWindowEraseBgSubclass(hwnd);
            DarkMode::setDarkWndNotifySafe(hwnd, true);
            ApplyModernWindowAttributes(hwnd);
            InitializeXamlIsland(hwnd);
        }
        catch (const winrt::hresult_error& error) {
            g_createError = L"Unable to open the XAML Islands dialog. This requires Windows 10 version 1903 or later and the WinRT XAML hosting runtime.\n\nHRESULT: " +
                std::to_wstring(static_cast<uint32_t>(static_cast<int32_t>(error.code())));
            g_logger.log(__FUNCTION__, Logger::Level::Error, g_createError.c_str());
            return -1;
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
        auto* state = reinterpret_cast<XamlDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete state;

        if (g_dialogWindow == hwnd) {
            g_dialogWindow = nullptr;
        }
        return 0;
    }

    default:
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
