#include "Logger.h"
#include "PeerDisplay.h"
#include "PeerManager.h"
#include "ClipboardActivityStore.h"
#include "clipp-win32-darkmode32/DMSubclass.h"
#include "ClippPage.h"
#include "NetworkPage.h"
#include "LogsPage.h"
#include "AboutPage.h"
#include "SettingsPage.h"
#include "AutoStart.h"
#include "resource.h"
#include "platform/uistrings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <limits>
#include <string>

#include <Windows.h>
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

#include "xaml_dialog.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "windowsapp.lib")

extern PeerManager g_peerManager;
extern PeerDisplay g_peerDisplay;
extern ClipboardActivityStore g_clipboardActivityStore;
extern Logger g_logger;

namespace {

constexpr wchar_t kDialogClassName[] = L"ClippMainXamlDialog";
constexpr double kDialogDefaultClientWidthDips = 820;
constexpr double kDialogDefaultClientHeightDips = 580;
constexpr double kDialogMinClientWidthDips = 620;
constexpr double kDialogMinClientHeightDips = 420;

void EnsureWinRtApartmentInitialized() {
    static bool apartmentInitialized = false;

    if (!apartmentInitialized) {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        apartmentInitialized = true;
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

void AliasThemeBrush(
    winrt::Windows::UI::Xaml::ResourceDictionary const& resources,
    const wchar_t* targetResourceName,
    const wchar_t* sourceResourceName)
{
    if (const auto brush = LookupThemeBrush(sourceResourceName)) {
        resources.Insert(
            winrt::box_value(winrt::hstring{ targetResourceName }),
            brush);
    }
}

void ApplyTextControlThemeResources(winrt::Windows::UI::Xaml::FrameworkElement const& element) {
    struct BrushAlias {
        const wchar_t* target;
        const wchar_t* source;
    };

    const BrushAlias aliases[] = {
        { L"TextControlBackgroundFocused", L"TextControlBackground" },
        { L"TextControlForegroundFocused", L"TextControlForeground" },
        { L"TextControlPlaceholderForegroundFocused", L"TextControlPlaceholderForeground" },
    };

    const auto resources = element.Resources();
    for (const auto& alias : aliases) {
        AliasThemeBrush(resources, alias.target, alias.source);
    }
}

void ApplyModernWindowAttributes(HWND hwnd) {
    BOOL useDarkTitleBar = DarkMode::isEnabled() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkTitleBar, sizeof(useDarkTitleBar));
    DwmSetWindowAttribute(hwnd, 19 /* DWMWA_USE_IMMERSIVE_DARK_MODE before Windows 10 20H1 */, &useDarkTitleBar, sizeof(useDarkTitleBar));

    const DWORD cornerPreferenceRound = 2;
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPreferenceRound, sizeof(cornerPreferenceRound));
}

int DipsToPixels(double dips, UINT dpi) {
    return static_cast<int>(std::ceil(dips * dpi / USER_DEFAULT_SCREEN_DPI));
}

class MainXamlDialog {
public:
    MainXamlDialog() = default;
    ~MainXamlDialog() {
        Destroy();
        if (xamlManager_) {
            xamlManager_.Close();
            xamlManager_ = nullptr;
        }
    }

    MainXamlDialog(const MainXamlDialog&) = delete;
    MainXamlDialog& operator=(const MainXamlDialog&) = delete;

    void Show(HWND owner) {
        try {
            owner_ = owner;
            RegisterDialogClass(GetModuleHandleW(nullptr));
            if (!hwnd_) {
                createError_.clear();
                Create(owner);
            }

            if (hwnd_) {
                SizeAndCenter(owner);
                ShowWindow(hwnd_, SW_SHOWNORMAL);
                SetForegroundWindow(hwnd_);
            } else if (!createError_.empty()) {
            DarkMode::DarkMessageBox(owner, createError_.c_str(), CLP_W(CLP_UI_APP_NAME), MB_ICONERROR | MB_OK);
            }
        } catch (const winrt::hresult_error& error) {
            const std::wstring message = L"Unable to open the XAML Islands dialog. HRESULT: " + std::to_wstring(error.code());
            g_logger.log(__FUNCTION__, Logger::Level::Error, message.c_str());
            DarkMode::DarkMessageBox(owner, message.c_str(), CLP_W(CLP_UI_APP_NAME), MB_ICONERROR | MB_OK);
        }
    }

    bool PreTranslateMessage(MSG* msg) {
        if (!hwnd_ || !msg || !xamlSource_) {
            return false;
        }
        auto nativeSource = xamlSource_.as<IDesktopWindowXamlSourceNative2>();
        BOOL handled = FALSE;
        return SUCCEEDED(nativeSource->PreTranslateMessage(msg, &handled)) && handled;
    }

    void Destroy() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

private:
    enum class PageID {
        Clipp = 0,
        Network = 1,
        Settings = 2,
        Logs = 3,
        About = 4,
    };

    struct MenuItemDefinition {
        const wchar_t* glyph;
        const wchar_t* label;
    };

    void Create(HWND owner) {
        hwnd_ = CreateWindowExW(
            WS_EX_APPWINDOW,
            kDialogClassName,
            CLP_W(CLP_UI_APP_NAME),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            820,
            580,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            this);

        if (!hwnd_ && createError_.empty()) {
            createError_ = L"Unable to create the Clipp dialog window.";
        }
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE:
            try {
                DarkMode::setWindowEraseBgSubclass(hwnd_);
                DarkMode::setDarkWndNotifySafe(hwnd_, true);
                ApplyModernWindowAttributes(hwnd_);
                InitializeXamlIsland();
            }
            catch (const winrt::hresult_error& error) {
                createError_ = L"Unable to open the XAML Islands dialog. This requires Windows 10 version 1903 or later and the WinRT XAML hosting runtime.\n\nHRESULT: " +
                    std::to_wstring(static_cast<uint32_t>(static_cast<int32_t>(error.code())));
                g_logger.log(__FUNCTION__, Logger::Level::Error, createError_.c_str());
                return -1;
            }
            return 0;

        case WM_SHOWWINDOW:
            if (wParam) {
                if (clippPage_) {
                    clippPage_->OnShown();
                }
                if (networkPage_) {
                    networkPage_->OnShown();
                }
                if (logsPage_) {
                    logsPage_->OnShown();
                }
                if (settingsPage_) {
                    settingsPage_->OnShown();
                }
            } else {
                if (clippPage_) {
                    clippPage_->OnHidden();
                }
                if (networkPage_) {
                    networkPage_->OnHidden();
                }
                if (logsPage_) {
                    logsPage_->OnHidden();
                }
            }
            return 0;

        case WM_SIZE:
            ResizeXamlHost();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindow(hwnd_);
            const RECT minWindowRect = WindowRectForClientSize(
                DipsToPixels(kDialogMinClientWidthDips, dpi),
                DipsToPixels(kDialogMinClientHeightDips, dpi));
            minMaxInfo->ptMinTrackSize.x = minWindowRect.right - minWindowRect.left;
            minMaxInfo->ptMinTrackSize.y = minWindowRect.bottom - minWindowRect.top;
            return 0;
        }

        case WM_CLOSE:
            ShowWindow(hwnd_, SW_HIDE);
            return 0;

        case WM_DESTROY:
            if (clippPage_) {
                clippPage_->OnDestroy();
            }
            if (networkPage_) {
                networkPage_->OnDestroy();
            }
            if (logsPage_) {
                logsPage_->OnDestroy();
            }
            clippPage_.reset();
            networkPage_.reset();
            settingsPage_.reset();
            logsPage_.reset();
            aboutPage_.reset();
            xamlSource_ = nullptr;
            xamlHost_ = nullptr;
            return 0;

        default:
            if (msg == derivedKeyMessage_) {
                auto* result = reinterpret_cast<const KeyManager::NetworkKey*>(wParam);
                if (networkPage_) {
                    networkPage_->OnDerivedKey(result);
                }
                return 0;
            }

            if (msg == peerDisplayUpdateMessage_) {
                if (networkPage_) {
                    networkPage_->OnPeerDisplayUpdate();
                }
                return 0;
            }

            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
    }

    void InitializeXamlIsland() {
        EnsureWinRtApartmentInitialized();
        if (!xamlManager_) {
            xamlManager_ = winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
        }

        xamlSource_ = winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource{};
        auto nativeSource = xamlSource_.as<IDesktopWindowXamlSourceNative>();
        winrt::check_hresult(nativeSource->AttachToWindow(hwnd_));
        winrt::check_hresult(nativeSource->get_WindowHandle(&xamlHost_));

        xamlSource_.Content(BuildShell());
        ResizeXamlHost();
    }

    winrt::Windows::UI::Xaml::Controls::Grid BuildShell() {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;
        using namespace winrt::Windows::UI::Xaml::Media;

        clippPage_ = std::make_unique<ClippPage>(g_clipboardActivityStore);
        networkPage_ = std::make_unique<NetworkPage>(hwnd_, derivedKeyMessage_, peerDisplayUpdateMessage_, g_peerDisplay, g_peerManager);
        settingsPage_ = std::make_unique<SettingsPage>();
        logsPage_ = std::make_unique<LogsPage>();
        aboutPage_ = std::make_unique<AboutPage>();

        Grid root;
        root.HorizontalAlignment(HorizontalAlignment::Stretch);
        root.VerticalAlignment(VerticalAlignment::Stretch);
        root.RequestedTheme(GetCurrentXamlTheme());
        ApplyTextControlThemeResources(root);
        root.Background(GetThemeBackgroundBrush());

        ColumnDefinition menuColumn;
        menuColumn.Width(GridLength{ 148, GridUnitType::Pixel });
        ColumnDefinition contentColumn;
        contentColumn.Width(GridLength{ 1, GridUnitType::Star });
        root.ColumnDefinitions().Append(menuColumn);
        root.ColumnDefinitions().Append(contentColumn);

        ListBox menu;
        menu.HorizontalAlignment(HorizontalAlignment::Stretch);
        menu.VerticalAlignment(VerticalAlignment::Stretch);
        menu.Padding(ThicknessHelper::FromLengths(8, 16, 8, 16));
        menu.SelectionMode(SelectionMode::Single);
        menu.Background(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(24, 127, 127, 127)));

        const MenuItemDefinition menuItems[] = {
            { L"\xE77F", CLP_W(CLP_UI_APP_NAME) },
            { L"\xE968", CLP_W(CLP_UI_NETWORK) },
            { L"\xE713", CLP_W(CLP_UI_SETTINGS) },
            { L"\xE8A5", CLP_W(CLP_UI_LOGS) },
            { L"\xE946", CLP_W(CLP_UI_ABOUT) },
        };

        for (const auto& menuItem : menuItems) {
            menu.Items().Append(CreateMenuItem(menuItem));
        }

        contentPresenter_ = ContentControl();
        contentPresenter_.HorizontalAlignment(HorizontalAlignment::Stretch);
        contentPresenter_.VerticalAlignment(VerticalAlignment::Stretch);
        contentPresenter_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        contentPresenter_.VerticalContentAlignment(VerticalAlignment::Stretch);
        contentPresenter_.Content(clippPage_->View());

        StackPanel sidebarActions;
        sidebarActions.Orientation(Orientation::Vertical);
        sidebarActions.Padding(ThicknessHelper::FromLengths(8, 0, 8, 16));
        sidebarActions.Spacing(8);

        Button minimizeButton = CreateSidebarButton(L"Minimize to Tray");
        minimizeButton.Click([this](auto const&, auto const&) {
            MinimizeToTray();
        });

        Button exitButton = CreateSidebarButton(CLP_W(CLP_UI_EXIT_CLIPP));
        exitButton.Click([this](auto const&, auto const&) {
            ExitApplication();
        });

        sidebarActions.Children().Append(minimizeButton);
        sidebarActions.Children().Append(exitButton);

        Grid sidebar;
        sidebar.HorizontalAlignment(HorizontalAlignment::Stretch);
        sidebar.VerticalAlignment(VerticalAlignment::Stretch);
        sidebar.Background(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(24, 127, 127, 127)));
        RowDefinition menuRow;
        menuRow.Height(GridLength{ 1, GridUnitType::Star });
        RowDefinition actionsRow;
        actionsRow.Height(GridLength{ 1, GridUnitType::Auto });
        sidebar.RowDefinitions().Append(menuRow);
        sidebar.RowDefinitions().Append(actionsRow);

        Grid::SetRow(menu, 0);
        Grid::SetRow(sidebarActions, 1);
        sidebar.Children().Append(menu);
        sidebar.Children().Append(sidebarActions);

        menu.SelectionChanged([this](auto const& sender, auto const&) {
            auto menu = sender.as<ListBox>();
            ShowPage(static_cast<PageID>(menu.SelectedIndex()));
        });
        menu.SelectedIndex(0);

        Grid::SetColumn(sidebar, 0);
        Grid::SetColumn(contentPresenter_, 1);
        root.Children().Append(sidebar);
        root.Children().Append(contentPresenter_);
        return root;
    }

    winrt::Windows::UI::Xaml::Controls::Button CreateSidebarButton(const wchar_t* label) {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;

        Button button;
        button.Content(winrt::box_value(winrt::hstring{ label }));
        button.HorizontalAlignment(HorizontalAlignment::Stretch);
        button.HorizontalContentAlignment(HorizontalAlignment::Center);
        button.Padding(ThicknessHelper::FromLengths(8, 6, 8, 6));
        return button;
    }

    winrt::Windows::UI::Xaml::Controls::ListBoxItem CreateMenuItem(const MenuItemDefinition& menuItem) {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;
        using namespace winrt::Windows::UI::Xaml::Media;

        FontIcon icon;
        icon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
        icon.Glyph(menuItem.glyph);
        icon.FontSize(16);
        icon.Width(20);
        icon.VerticalAlignment(VerticalAlignment::Center);

        TextBlock label;
        label.Text(menuItem.label);
        label.VerticalAlignment(VerticalAlignment::Center);

        StackPanel content;
        content.Orientation(Orientation::Horizontal);
        content.Spacing(10);
        content.VerticalAlignment(VerticalAlignment::Center);
        content.Children().Append(icon);
        content.Children().Append(label);

        ListBoxItem item;
        item.Content(content);
        item.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
        return item;
    }

    void ShowPage(PageID pageID) {
        if (!contentPresenter_) {
            return;
        }

        switch (pageID) {
        case PageID::Clipp:
            contentPresenter_.Content(clippPage_->View());
            break;
        case PageID::Network:
            contentPresenter_.Content(networkPage_->View());
            break;
        case PageID::Settings:
            settingsPage_->OnShown();
            contentPresenter_.Content(settingsPage_->View());
            break;
        case PageID::Logs:
            contentPresenter_.Content(logsPage_->View());
            break;
        case PageID::About:
            contentPresenter_.Content(aboutPage_->View());
            break;
        }
    }

    void MinimizeToTray() {
        if (hwnd_) {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }

    void ExitApplication() {
        UnregisterClippAutoStart();

        if (owner_ && IsWindow(owner_)) {
            PostMessageW(owner_, WM_CLOSE, 0, 0);
            return;
        }

        Destroy();
        PostQuitMessage(0);
    }

    RECT WindowRectForClientSize(int clientWidth, int clientHeight) const {
        RECT rect{ 0, 0, clientWidth, clientHeight };
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
        const BOOL hasMenu = GetMenu(hwnd_) != nullptr;
        const UINT dpi = GetDpiForWindow(hwnd_);

        if (!AdjustWindowRectExForDpi(&rect, style, hasMenu, exStyle, dpi)) {
            AdjustWindowRectEx(&rect, style, hasMenu, exStyle);
        }

        return rect;
    }

    SIZE DesiredClientSizePixels() const {
        const UINT dpi = GetDpiForWindow(hwnd_);
        double widthDips = kDialogDefaultClientWidthDips;
        double heightDips = kDialogDefaultClientHeightDips;

        if (xamlSource_) {
            const auto content = xamlSource_.Content();
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

    HMONITOR DialogMonitor(HWND owner) const {
        if (owner) {
            if (HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONULL)) {
                return monitor;
            }
        }

        if (HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONULL)) {
            return monitor;
        }

        POINT cursor{};
        if (GetCursorPos(&cursor)) {
            return MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        }

        return MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    }

    void SizeAndCenter(HWND owner) {
        const SIZE desiredClientSize = DesiredClientSizePixels();
        RECT windowRect = WindowRectForClientSize(desiredClientSize.cx, desiredClientSize.cy);
        int windowWidth = windowRect.right - windowRect.left;
        int windowHeight = windowRect.bottom - windowRect.top;

        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        GetMonitorInfoW(DialogMonitor(owner), &monitorInfo);

        const RECT workArea = monitorInfo.rcWork;
        const int workWidth = workArea.right - workArea.left;
        const int workHeight = workArea.bottom - workArea.top;
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int margin = DipsToPixels(24, dpi);

        windowWidth = (std::min)(windowWidth, (std::max)(1, workWidth - (margin * 2)));
        windowHeight = (std::min)(windowHeight, (std::max)(1, workHeight - (margin * 2)));

        const int x = workArea.left + ((workWidth - windowWidth) / 2);
        const int y = workArea.top + ((workHeight - windowHeight) / 2);

        SetWindowPos(hwnd_, nullptr, x, y, windowWidth, windowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        ResizeXamlHost();
    }

    void ResizeXamlHost() const {
        if (!xamlHost_) {
            return;
        }

        RECT clientRect{};
        GetClientRect(hwnd_, &clientRect);
        SetWindowPos(
            xamlHost_,
            nullptr,
            0,
            0,
            clientRect.right - clientRect.left,
            clientRect.bottom - clientRect.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(xamlHost_, SW_SHOW);
    }

    static void RegisterDialogClass(HINSTANCE hInstance) {
        static bool registered = false;
        if (registered) {
            return;
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_CLIPP_ICON));
        wc.hIconSm = static_cast<HICON>(LoadImageW(
            hInstance,
            MAKEINTRESOURCEW(IDI_CLIPP_ICON),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR | LR_SHARED));
        if (!wc.hIcon) {
            wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        }
        if (!wc.hIconSm) {
            wc.hIconSm = wc.hIcon;
        }
        wc.lpszClassName = kDialogClassName;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

        RegisterClassExW(&wc);
        registered = true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        MainXamlDialog* dialog = nullptr;

        if (msg == WM_NCCREATE) {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = reinterpret_cast<MainXamlDialog*>(createStruct->lpCreateParams);
            dialog->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        } else {
            dialog = reinterpret_cast<MainXamlDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (!dialog) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        const LRESULT result = dialog->HandleMessage(msg, wParam, lParam);
        if (msg == WM_NCDESTROY) {
            dialog->hwnd_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return result;
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND xamlHost_ = nullptr;
    std::wstring createError_;
    winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager xamlManager_{ nullptr };
    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource xamlSource_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::ContentControl contentPresenter_{ nullptr };
    std::unique_ptr<ClippPage> clippPage_;
    std::unique_ptr<NetworkPage> networkPage_;
    std::unique_ptr<SettingsPage> settingsPage_;
    std::unique_ptr<LogsPage> logsPage_;
    std::unique_ptr<AboutPage> aboutPage_;
    UINT derivedKeyMessage_ = RegisterWindowMessageW(L"ClippDerivedKeyNotification");
    UINT peerDisplayUpdateMessage_ = RegisterWindowMessageW(L"ClippPeerDisplayUpdate");
};

std::unique_ptr<MainXamlDialog> g_dialog;

} // namespace

void ShowClippMainDialog(HWND owner) {
    if (!g_dialog) {
        g_dialog = std::make_unique<MainXamlDialog>();
    }
    g_dialog->Show(owner);
}

bool ClippMainDialogPreTranslateMessage(MSG* msg) {
    return g_dialog ? g_dialog->PreTranslateMessage(msg) : false;
}

void CloseClippMainDialog() {
    g_dialog.reset();
}
