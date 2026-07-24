#include "platform.h"

#include "PopupWindow.h"

#include "ClipboardActions.h"
#include "ClipboardActivityStore.h"
#include "ClipboardFormat.h"
#include "Logger.h"
#include "PopupModel.h"
#include "RegisterStore.h"
#include "RegisterWire.h"
#include "Settings.h"
#include "clipp-win32-darkmode32/DMSubclass.h"
#include "platform/uiClippPage.h"
#include "platform/uistrings.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <dwmapi.h>
#include <unknwn.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

#include "XamlImage.h"

#pragma comment(lib, "dwmapi.lib")

extern ClipboardActivityStore g_clipboardActivityStore;
extern Logger g_logger;

namespace {

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

constexpr wchar_t kPopupClassName[] = L"ClippPopupWindow";
constexpr wchar_t kToastClassName[] = L"ClippPopupToast";
constexpr wchar_t kPreviewClassName[] = L"ClippPopupPreview";
// One column while only the clipboard stream exists; the registers column
// (left of it) widens the popup and brings the group labels with it.
constexpr double kPopupWidthDips = 420;
constexpr double kPopupWidthTwoColDips = 700;
constexpr double kPopupHeightDips = 540;
// XAML row construction is the expensive part of a re-render; cap what one
// filter state shows (per column) and say so with a hint row instead of
// silently cropping.
constexpr std::size_t kMaxRenderedRows = 40;
// Register content previews mirror the history display's cap (the file-local
// kMaxTextPreviewCharacters in ClipboardActivityDisplay.cpp): find matches
// against this window, the flyout against the full value.
constexpr std::size_t kRegisterPreviewChars = 640;
// Preview flyout geometry: content-sized, up to these caps.
constexpr double kPreviewMaxTextWidthDips = 360;
constexpr double kPreviewMaxHeightDips = 520;
// Row text is one ellipsized line; text this short fits it and earns no flyout.
constexpr std::size_t kRowFitChars = 60;
// Re-windowing: context kept ahead of the first match in a row / the flyout,
// and how much total text the flyout shows. The match is visible by
// construction — no scrolling machinery to go wrong.
constexpr std::size_t kRowMatchLeadChars = 12;
constexpr std::size_t kPreviewLeadChars = 400;
constexpr std::size_t kPreviewWindowChars = 2500;
// A one-letter filter over a big text can hit thousands of times; the
// highlighter caps out rather than drowning the renderer.
constexpr std::size_t kMaxHighlightRanges = 200;

int DipsToPixels(double dips, UINT dpi) {
    return static_cast<int>(std::ceil(dips * dpi / USER_DEFAULT_SCREEN_DPI));
}

ElementTheme CurrentTheme() {
    return DarkMode::isEnabled() ? ElementTheme::Dark : ElementTheme::Light;
}

SolidColorBrush ArgbBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b));
}

Brush PopupBackgroundBrush() {
    // Solid (not theme-resource) so the borderless window reads as one crisp
    // surface; tones match the settings window's chrome.
    return DarkMode::isEnabled() ? ArgbBrush(255, 32, 32, 32) : ArgbBrush(255, 243, 243, 243);
}

Brush LookupThemeBrush(const wchar_t* resourceName) {
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

// Same fix the settings window carries: focused text controls otherwise flip
// to their light-theme brushes inside an island. Alias the focused-state
// resources to the unfocused ones.
void ApplyTextControlThemeResources(FrameworkElement const& element) {
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
        if (const auto brush = LookupThemeBrush(alias.source)) {
            resources.Insert(winrt::box_value(winrt::hstring{ alias.target }), brush);
        }
    }
}

wchar_t FoldAscii(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
}

// Non-overlapping, ASCII-case-insensitive occurrences of `needle` — the same
// folding the model's filter uses, so what matched is what lights up.
std::vector<std::size_t> FindMatches(const std::wstring& text, const std::wstring& needle) {
    std::vector<std::size_t> matches;
    if (needle.empty() || needle.size() > text.size()) {
        return matches;
    }
    for (std::size_t start = 0; start + needle.size() <= text.size(); ++start) {
        bool hit = true;
        for (std::size_t i = 0; i < needle.size(); ++i) {
            if (FoldAscii(text[start + i]) != FoldAscii(needle[i])) {
                hit = false;
                break;
            }
        }
        if (hit) {
            matches.push_back(start);
            if (matches.size() >= kMaxHighlightRanges) {
                break;
            }
            start += needle.size() - 1;
        }
    }
    return matches;
}

// Amber find-highlight with forced dark text: readable over both themes.
void HighlightMatches(TextBlock const& block, const std::wstring& text, const std::wstring& filter) {
    block.TextHighlighters().Clear();
    if (filter.empty()) {
        return;
    }
    const auto matches = FindMatches(text, filter);
    if (matches.empty()) {
        return;
    }
    winrt::Windows::UI::Xaml::Documents::TextHighlighter highlighter;
    highlighter.Background(ArgbBrush(150, 255, 185, 0));
    highlighter.Foreground(ArgbBrush(255, 0, 0, 0));
    for (const auto start : matches) {
        highlighter.Ranges().Append(winrt::Windows::UI::Xaml::Documents::TextRange{
            static_cast<int32_t>(start), static_cast<int32_t>(filter.size()) });
    }
    block.TextHighlighters().Append(highlighter);
}

std::wstring RelativeAgeText(std::chrono::system_clock::time_point when) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const long long secs = when <= now ? duration_cast<seconds>(now - when).count() : 0;
    if (secs < 5)      return L"just now";
    if (secs < 60)     return std::to_wstring(secs) + L" seconds ago";
    if (secs < 120)    return L"a minute ago";
    if (secs < 3600)   return std::to_wstring(secs / 60) + L" minutes ago";
    if (secs < 7200)   return L"an hour ago";
    if (secs < 86400)  return std::to_wstring(secs / 3600) + L" hours ago";
    if (secs < 172800) return L"yesterday";
    return std::to_wstring(secs / 86400) + L" days ago";
}

// Register HLCs carry Unix wall-clock milliseconds.
std::wstring RelativeAgeText(uint64_t unixWallMs) {
    return RelativeAgeText(std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds{ unixWallMs }) });
}

// Toolbar button: icon-only with a tooltip naming the action and its key.
// Mouse-only by design (IsTabStop off — the filter box keeps the keyboard,
// and every action has a key equivalent).
Button MakeToolbarButton(const wchar_t* glyph, const wchar_t* tooltip) {
    FontIcon icon;
    icon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    icon.Glyph(winrt::hstring{ glyph });
    icon.FontSize(14);
    Button button;
    button.Content(icon);
    button.Padding(ThicknessHelper::FromLengths(8, 5, 8, 5));
    button.MinWidth(0);
    button.MinHeight(0);
    button.IsTabStop(false);
    ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring{ tooltip }));
    return button;
}

// Trailing shell newlines (a piped `clipp copy`) must not force a flyout for
// one short line: the fits-in-a-row decision looks at the whitespace-trimmed
// core. The flyout, when it does earn its keep, shows the value untrimmed.
bool TextFitsInRow(const std::wstring& full) {
    const auto first = full.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return true;  // nothing but whitespace: nothing a flyout could add
    }
    const auto last = full.find_last_not_of(L" \t\r\n");
    const std::wstring_view core(full.data() + first, last - first + 1);
    return core.find(L'\n') == std::wstring_view::npos && core.size() <= kRowFitChars;
}

// ---- companion windows ----
// Both are WS_EX_NOACTIVATE satellites of the popup: they can never take the
// keyboard home away from the filter box, never trip the popup's
// light-dismiss (they don't activate at all), and live entirely outside the
// popup's own layout.

// Coaching toast: a text pill floating ABOVE the popup, outside its bounds.
// Plain GDI — no island, no XAML, nothing to fight.
class ToastWindow {
public:
    void ShowAbove(HWND popupWindow, const wchar_t* text) {
        text_ = text;
        EnsureCreated();
        if (hwnd_ == nullptr) {
            return;
        }

        const UINT dpi = GetDpiForWindow(popupWindow);
        EnsureFont(dpi);

        RECT measure{};
        if (HDC hdc = GetDC(hwnd_)) {
            const HGDIOBJ old = SelectObject(hdc, font_);
            DrawTextW(hdc, text_.c_str(), -1, &measure, DT_CALCRECT | DT_SINGLELINE);
            SelectObject(hdc, old);
            ReleaseDC(hwnd_, hdc);
        }
        const int width = (measure.right - measure.left) + MulDiv(14, dpi, 96) * 2;
        const int height = (measure.bottom - measure.top) + MulDiv(7, dpi, 96) * 2;

        RECT popupRect{};
        GetWindowRect(popupWindow, &popupRect);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromWindow(popupWindow, MONITOR_DEFAULTTONEAREST), &info);
        const int x = popupRect.left + ((popupRect.right - popupRect.left) - width) / 2;
        int y = popupRect.top - height - MulDiv(10, dpi, 96);
        if (y < info.rcWork.top) {
            y = info.rcWork.top;  // popup hugs the screen top: sit flush
        }
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void Hide() {
        if (hwnd_ != nullptr && IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }

    void Destroy() {
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (font_ != nullptr) {
            DeleteObject(font_);
            font_ = nullptr;
        }
    }

private:
    void EnsureCreated() {
        if (hwnd_ != nullptr) {
            return;
        }
        const HINSTANCE hInstance = GetModuleHandleW(nullptr);
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WndProc;
            wc.hInstance = hInstance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kToastClassName;
            RegisterClassExW(&wc);
            registered = true;
        }
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kToastClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, this);
        if (hwnd_ != nullptr) {
            const DWORD cornerRoundSmall = 3 /*DWMWCP_ROUNDSMALL*/;
            DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                &cornerRoundSmall, sizeof(cornerRoundSmall));
        }
    }

    void EnsureFont(UINT dpi) {
        if (font_ != nullptr && fontDpi_ == dpi) {
            return;
        }
        if (font_ != nullptr) {
            DeleteObject(font_);
        }
        font_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        fontDpi_ = dpi;
    }

    void Paint() {
        PAINTSTRUCT ps{};
        const HDC hdc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const HBRUSH background = CreateSolidBrush(RGB(45, 45, 45));
        FillRect(hdc, &client, background);
        DeleteObject(background);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        const HGDIOBJ old = SelectObject(hdc, font_);
        DrawTextW(hdc, text_.c_str(), -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        EndPaint(hwnd_, &ps);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ToastWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<ToastWindow*>(createStruct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ToastWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        switch (msg) {
        case WM_PAINT:
            if (self != nullptr) {
                self->Paint();
                return 0;
            }
            break;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    UINT fontDpi_ = 0;
    std::wstring text_;
};

// Preview flyout: summoned only when the selection holds more than its row can
// show (an image, or long/multiline text). Pinned beside the popup at the
// selected row's height, sized to its content up to a cap. Self-managed rather
// than a XAML Flyout: same look and transience, none of the focus-steal /
// per-keystroke reopen churn / light-dismiss fights.
class PreviewWindow {
public:
    void ShowText(HWND popupWindow, int anchorScreenY, const std::wstring& text,
                  const std::wstring& filter, bool preferLeft) {
        EnsureCreated();
        if (hwnd_ == nullptr) {
            return;
        }
        image_.Visibility(Visibility::Collapsed);
        image_.Source(nullptr);
        text_.Visibility(Visibility::Visible);
        text_.Text(winrt::hstring{ text });
        HighlightMatches(text_, text, filter);
        popupWindow_ = popupWindow;
        anchorY_ = anchorScreenY;
        preferLeft_ = preferLeft;
        PositionAndShow();
    }

    void ShowImage(HWND popupWindow, int anchorScreenY,
                   const std::shared_ptr<const std::vector<unsigned char>>& bytes,
                   bool preferLeft) {
        EnsureCreated();
        if (hwnd_ == nullptr || !bytes) {
            return;
        }
        text_.Visibility(Visibility::Collapsed);
        text_.Text(L"");
        text_.TextHighlighters().Clear();
        image_.Visibility(Visibility::Visible);
        image_.Source(BitmapFromImageBytes(*bytes, static_cast<int32_t>(kPreviewMaxTextWidthDips)));
        popupWindow_ = popupWindow;
        anchorY_ = anchorScreenY;
        preferLeft_ = preferLeft;
        PositionAndShow();  // provisional; ImageOpened re-runs with the real aspect
    }

    void Hide() {
        if (hwnd_ != nullptr && IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }

    void Destroy() {
        if (xamlSource_) {
            xamlSource_.Close();
            xamlSource_ = nullptr;
        }
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

private:
    void EnsureCreated() {
        if (hwnd_ != nullptr) {
            return;
        }
        const HINSTANCE hInstance = GetModuleHandleW(nullptr);
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WndProc;
            wc.hInstance = hInstance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kPreviewClassName;
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            RegisterClassExW(&wc);
            registered = true;
        }
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kPreviewClassName, L"", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, hInstance, this);
        if (hwnd_ == nullptr) {
            return;
        }
        DarkMode::setWindowEraseBgSubclass(hwnd_);
        const DWORD cornerRound = 2 /*DWMWCP_ROUND*/;
        DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
            &cornerRound, sizeof(cornerRound));

        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
        } catch (...) {
        }
        if (!xamlManager_) {
            xamlManager_ = Hosting::WindowsXamlManager::InitializeForCurrentThread();
        }
        xamlSource_ = Hosting::DesktopWindowXamlSource{};
        auto nativeSource = xamlSource_.as<IDesktopWindowXamlSourceNative>();
        winrt::check_hresult(nativeSource->AttachToWindow(hwnd_));
        winrt::check_hresult(nativeSource->get_WindowHandle(&xamlHost_));
        xamlSource_.Content(BuildContent());
    }

    Border BuildContent() {
        image_ = Image();
        image_.Stretch(Stretch::Uniform);
        image_.MaxWidth(kPreviewMaxTextWidthDips);
        image_.HorizontalAlignment(HorizontalAlignment::Left);
        image_.Visibility(Visibility::Collapsed);
        image_.ImageOpened([this](auto const&, auto const&) {
            // Decoded dimensions are in: re-fit the window to the real aspect.
            if (hwnd_ != nullptr && IsWindowVisible(hwnd_)) {
                PositionAndShow();
            }
        });

        text_ = TextBlock();
        text_.FontSize(13);
        text_.TextWrapping(TextWrapping::Wrap);
        text_.MaxWidth(kPreviewMaxTextWidthDips);
        text_.HorizontalAlignment(HorizontalAlignment::Left);
        text_.IsTextSelectionEnabled(false);

        StackPanel stack;
        stack.Spacing(6);
        stack.HorizontalAlignment(HorizontalAlignment::Left);
        stack.Children().Append(image_);
        stack.Children().Append(text_);

        root_ = Border();
        root_.RequestedTheme(CurrentTheme());
        root_.Background(PopupBackgroundBrush());
        root_.BorderBrush(ArgbBrush(64, 127, 127, 127));
        root_.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
        root_.Padding(ThicknessHelper::FromLengths(10, 8, 10, 8));
        root_.Child(stack);
        return root_;
    }

    void PositionAndShow() {
        if (hwnd_ == nullptr || !root_ || popupWindow_ == nullptr) {
            return;
        }
        // Content-sized up to the caps: measure the XAML tree at the maximum
        // box and take what it wants.
        root_.Measure(winrt::Windows::Foundation::Size{
            static_cast<float>(kPreviewMaxTextWidthDips + 22),
            static_cast<float>(kPreviewMaxHeightDips) });
        const auto desired = root_.DesiredSize();

        const UINT dpi = GetDpiForWindow(popupWindow_);
        int width = DipsToPixels((std::max)(140.0f, desired.Width), dpi);
        int height = DipsToPixels(
            (std::min)(static_cast<double>(desired.Height), kPreviewMaxHeightDips), dpi);
        height = (std::max)(height, DipsToPixels(44, dpi));

        RECT popupRect{};
        GetWindowRect(popupWindow_, &popupRect);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromWindow(popupWindow_, MONITOR_DEFAULTTONEAREST), &info);
        // Register rows open towards their own column's side (the popup's
        // left); history rows towards the right. Either flips when the work
        // area runs out.
        const int gap = DipsToPixels(8, dpi);
        int x;
        if (preferLeft_) {
            x = popupRect.left - width - gap;
            if (x < info.rcWork.left) {
                x = popupRect.right + gap;
            }
        } else {
            x = popupRect.right + gap;
            if (x + width > info.rcWork.right) {
                x = popupRect.left - width - gap;
            }
        }
        int y = anchorY_;
        if (y + height > info.rcWork.bottom) {
            y = info.rcWork.bottom - height;
        }
        if (y < info.rcWork.top) {
            y = info.rcWork.top;
        }
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ResizeXamlHost();
    }

    void ResizeXamlHost() {
        if (xamlHost_ == nullptr || hwnd_ == nullptr) {
            return;
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        SetWindowPos(xamlHost_, nullptr, 0, 0,
            client.right - client.left, client.bottom - client.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        PreviewWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<PreviewWindow*>(createStruct->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SIZE:
            if (self != nullptr) {
                self->ResizeXamlHost();
                return 0;
            }
            break;
        case WM_NCDESTROY:
            if (self != nullptr) {
                self->hwnd_ = nullptr;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HWND hwnd_ = nullptr;
    HWND xamlHost_ = nullptr;
    HWND popupWindow_ = nullptr;
    int anchorY_ = 0;
    bool preferLeft_ = false;
    Hosting::WindowsXamlManager xamlManager_{ nullptr };
    Hosting::DesktopWindowXamlSource xamlSource_{ nullptr };
    Border root_{ nullptr };
    Image image_{ nullptr };
    TextBlock text_{ nullptr };
};

class PopupWindow {
public:
    void Toggle() {
        if (hwnd_ != nullptr && IsWindowVisible(hwnd_)) {
            Dismiss(/*restoreFocus=*/true);
        } else {
            Summon();
        }
    }

    bool PreTranslateMessage(MSG* msg) {
        if (hwnd_ == nullptr || !IsWindowVisible(hwnd_) || !xamlSource_) {
            return false;
        }
        // Wheel input doesn't reliably reach the island's ScrollViewer in this
        // hosting setup; when the cursor is over the popup, drive the list
        // scroll ourselves and swallow the message. The island's input HWND
        // receives wheel as POINTER messages on current Windows (the framework
        // enables mouse-in-pointer), so WM_MOUSEWHEEL alone never matches —
        // intercept both spellings. Delta lives in the same wParam word.
#ifndef WM_POINTERWHEEL
#define WM_POINTERWHEEL 0x024E
#endif
        if ((msg->message == WM_MOUSEWHEEL || msg->message == WM_POINTERWHEEL) && listScroll_) {
            RECT rect{};
            GetWindowRect(hwnd_, &rect);
            const POINT cursor{
                static_cast<LONG>(static_cast<SHORT>(LOWORD(msg->lParam))),
                static_cast<LONG>(static_cast<SHORT>(HIWORD(msg->lParam))),
            };
            if (PtInRect(&rect, cursor)) {
                DismissHintToast();
                const int delta = GET_WHEEL_DELTA_WPARAM(msg->wParam);
                constexpr double kPixelsPerNotch = 96.0;
                const auto target = WheelTarget();
                const double offset =
                    target.VerticalOffset() - (static_cast<double>(delta) / WHEEL_DELTA) * kPixelsPerNotch;
                target.ChangeView(nullptr,
                    winrt::Windows::Foundation::IReference<double>{ offset }, nullptr, true);
                return true;
            }
        }

        auto native2 = xamlSource_.try_as<IDesktopWindowXamlSourceNative2>();
        if (!native2) {
            return false;
        }
        BOOL translated = FALSE;
        if (FAILED(native2->PreTranslateMessage(msg, &translated))) {
            return false;
        }
        return translated == TRUE;
    }

    void Destroy() {
        EndActivityNotifications();
        toastWindow_.Destroy();
        previewWindow_.Destroy();
        if (xamlSource_) {
            xamlSource_.Close();
            xamlSource_ = nullptr;
        }
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

private:
    // ---- lifecycle ----

    void Summon() {
        if (hwnd_ == nullptr) {
            Create();
            if (hwnd_ == nullptr) {
                return;
            }
        }

        previousForeground_ = GetForegroundWindow();

        RebuildFromStores();
        UpdateColumnLayout();
        if (filterBox_) {
            filterBox_.Text(L"");  // fresh session; fires TextChanged -> SetFilter("")
        }
        RenderList();

        PositionOnCursorMonitor();
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        FocusFilterBox();

        // First-runs coaching: a pill ABOVE the popup (its own no-activate
        // window — zero popup real estate) on each of the first
        // PopupHintMaxShows summons; the first action retires it.
        if (g_settings.popupHintShownCount() < Settings::PopupHintMaxShows) {
            toastWindow_.ShowAbove(hwnd_, CLP_W(CLP_UI_POPUP_TOAST));
            g_settings.notePopupHintShown();
        }

        // The pre-show RenderList ran while the window was still hidden, so
        // the flyout's visibility guard suppressed it; give the initial
        // selection its preview now (deferred — the rows arrange first).
        SchedulePreviewFlyoutUpdate();

        BeginActivityNotifications();
    }

    // Keyboard must land in the filter box every single time, or the popup is
    // dead to arrows/Esc. Belt and suspenders: Win32 focus onto the island's
    // HWND, XAML focus navigated into the island, and a deferred explicit
    // Focus() once layout has settled.
    void FocusFilterBox() {
        if (xamlHost_ != nullptr) {
            SetFocus(xamlHost_);
        }
        if (xamlSource_) {
            try {
                xamlSource_.NavigateFocus(Hosting::XamlSourceFocusNavigationRequest(
                    Hosting::XamlSourceFocusNavigationReason::Programmatic));
            } catch (...) {
            }
        }
        if (dispatcher_ && filterBox_) {
            dispatcher_.TryEnqueue([this]() {
                if (filterBox_ && hwnd_ != nullptr && IsWindowVisible(hwnd_)) {
                    filterBox_.Focus(FocusState::Programmatic);
                }
            });
        }
    }

    void Dismiss(bool restoreFocus) {
        if (hwnd_ == nullptr || !IsWindowVisible(hwnd_)) {
            return;
        }
        if (editingRegister_.has_value()) {
            EndEditMode();  // silent cancel; the next summon rebuilds the rows
        }
        EndActivityNotifications();
        toastWindow_.Hide();
        previewWindow_.Hide();
        // Session-scoped peeks: anything revealed inside the popup is
        // forgotten the moment it hides.
        uiClippPage::ForgetAllPeekedItems();
        ShowWindow(hwnd_, SW_HIDE);
        if (restoreFocus && previousForeground_ != nullptr && IsWindow(previousForeground_)) {
            SetForegroundWindow(previousForeground_);
        }
        previousForeground_ = nullptr;
    }

    void Create() {
        RegisterPopupClass();
        const HINSTANCE hInstance = GetModuleHandleW(nullptr);
        CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kPopupClassName,
            L"Clipp",
            WS_POPUP,
            0, 0,
            DipsToPixels(kPopupWidthDips, USER_DEFAULT_SCREEN_DPI),
            DipsToPixels(kPopupHeightDips, USER_DEFAULT_SCREEN_DPI),
            nullptr, nullptr, hInstance, this);
        if (hwnd_ == nullptr) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, L"Popup window creation failed.");
            return;
        }

        // Same anti-flashbang the main dialog runs at WM_CREATE: the erase-bg
        // subclass paints the window in the dark dialog color instead of the
        // class's white brush while the island is still warming up. Installed
        // before the first ShowWindow, so no white frame ever reaches glass.
        DarkMode::setWindowEraseBgSubclass(hwnd_);
        DarkMode::setDarkWndNotifySafe(hwnd_, true);

        BOOL dark = DarkMode::isEnabled() ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        const DWORD cornerRound = 2 /*DWMWCP_ROUND*/;
        DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerRound, sizeof(cornerRound));

        InitializeXamlIsland();
    }

    void InitializeXamlIsland() {
        // The tray thread hosts the settings window's island too; the manager
        // and apartment are idempotent per thread.
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
        } catch (...) {
            // Already initialized on this thread — fine.
        }
        if (!xamlManager_) {
            xamlManager_ = Hosting::WindowsXamlManager::InitializeForCurrentThread();
        }
        if (!dispatcher_) {
            dispatcher_ = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
        }
        xamlSource_ = Hosting::DesktopWindowXamlSource{};
        auto nativeSource = xamlSource_.as<IDesktopWindowXamlSourceNative>();
        winrt::check_hresult(nativeSource->AttachToWindow(hwnd_));
        winrt::check_hresult(nativeSource->get_WindowHandle(&xamlHost_));
        xamlSource_.Content(BuildContent());
        ResizeXamlHost();
    }

    // ---- content ----

    Grid BuildContent() {
        Grid root;
        root.RequestedTheme(CurrentTheme());
        root.Background(PopupBackgroundBrush());
        ApplyTextControlThemeResources(root);

        // Rows: identity header, action toolbar, search field, columns.
        RowDefinition headerRow;
        headerRow.Height(GridLength{ 0, GridUnitType::Auto });
        RowDefinition toolbarRow;
        toolbarRow.Height(GridLength{ 0, GridUnitType::Auto });
        RowDefinition filterRow;
        filterRow.Height(GridLength{ 0, GridUnitType::Auto });
        RowDefinition listRow;
        listRow.Height(GridLength{ 1, GridUnitType::Star });
        root.RowDefinitions().Append(headerRow);
        root.RowDefinitions().Append(toolbarRow);
        root.RowDefinitions().Append(filterRow);
        root.RowDefinitions().Append(listRow);

        // Identity bar: a surprise borderless window on a stray keystroke
        // should say what it is, and offer an obvious way out.
        Grid header;
        header.Padding(ThicknessHelper::FromLengths(14, 10, 8, 0));
        ColumnDefinition titleColumn;
        titleColumn.Width(GridLength{ 1, GridUnitType::Star });
        ColumnDefinition closeColumn;
        closeColumn.Width(GridLength{ 0, GridUnitType::Auto });
        header.ColumnDefinitions().Append(titleColumn);
        header.ColumnDefinitions().Append(closeColumn);

        TextBlock title;
        title.Text(winrt::hstring{ CLP_W(CLP_UI_APP_NAME) L" — " CLP_W(CLP_UI_TRAY_POPUP) });
        title.FontSize(13);
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.Opacity(0.75);
        title.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(title, 0);
        header.Children().Append(title);

        FontIcon closeIcon;
        closeIcon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
        closeIcon.Glyph(L"\xE711");
        closeIcon.FontSize(12);
        Button closeButton;
        closeButton.Content(closeIcon);
        closeButton.Width(30);
        closeButton.Height(26);
        closeButton.MinWidth(0);
        closeButton.MinHeight(0);
        closeButton.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
        closeButton.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 0));
        closeButton.Background(ArgbBrush(0, 0, 0, 0));
        closeButton.IsTabStop(false);
        closeButton.Click([this](auto const&, auto const&) {
            Dismiss(/*restoreFocus=*/true);
        });
        Grid::SetColumn(closeButton, 1);
        header.Children().Append(closeButton);

        Grid::SetRow(header, 0);
        root.Children().Append(header);

        // The template's placeholder machinery is a lost cause inside an
        // island (its state brushes resolve to invisible colors), so the hint
        // is our own overlay TextBlock instead — visible exactly while the
        // filter is empty, in a color we control.
        filterBox_ = TextBox();
        filterBox_.Margin(ThicknessHelper::FromLengths(12, 12, 12, 8));
        // The race-free half of first-summon focus: on a cold open the island
        // content isn't in the live tree yet when Summon's deferred Focus()
        // runs (very visible under gflags heap checking, but the race exists
        // everywhere), so Focus() no-ops and the keyboard lands nowhere.
        // Loaded fires exactly when the box becomes focusable — take it then.
        filterBox_.Loaded([this](auto const&, auto const&) {
            if (filterBox_ && hwnd_ != nullptr && IsWindowVisible(hwnd_)) {
                filterBox_.Focus(FocusState::Programmatic);
            }
        });
        filterHint_ = TextBlock();
        filterHint_.Text(winrt::hstring{ CLP_W(CLP_UI_POPUP_FILTER_HINT) });
        filterHint_.Opacity(0.55);
        filterHint_.IsHitTestVisible(false);
        filterHint_.VerticalAlignment(VerticalAlignment::Center);
        filterHint_.Margin(ThicknessHelper::FromLengths(24, 12, 24, 8));
        filterBox_.TextChanged([this](auto const&, auto const&) {
            if (filterHint_) {
                filterHint_.Visibility(filterBox_.Text().empty()
                    ? Visibility::Visible : Visibility::Collapsed);
            }
            model_.SetFilter(std::wstring{ filterBox_.Text() });
            RenderList();
        });
        // PreviewKeyDown so navigation wins over the TextBox's own key
        // handling; the box keeps keyboard focus for the popup's whole life
        // (the launcher pattern) and the list is driven from here.
        filterBox_.PreviewKeyDown([this](auto const&, Input::KeyRoutedEventArgs const& args) {
            OnFilterKey(args);
        });
        Grid filterHost;
        filterHost.Children().Append(filterBox_);
        filterHost.Children().Append(filterHint_);
        Grid::SetRow(filterHost, 2);
        root.Children().Append(filterHost);

        // Action toolbar, above the search field: icon-only, tooltipped.
        // Save promotes the selected clipboard item into a register (enabled
        // only there); Rename and the privacy toggle act on registers only;
        // Copy mirrors Enter; Delete mirrors Del.
        StackPanel toolbar;
        toolbar.Orientation(Orientation::Horizontal);
        toolbar.Spacing(4);
        toolbar.Margin(ThicknessHelper::FromLengths(12, 6, 12, 0));
        saveButton_ = MakeToolbarButton(L"\xE74E", CLP_W(CLP_UI_POPUP_SAVE_TIP));
        saveButton_.Click([this](auto const&, auto const&) {
            SaveSelected();
        });
        copyButton_ = MakeToolbarButton(L"\xE8C8", CLP_W(CLP_UI_POPUP_COPY_TIP));
        copyButton_.Click([this](auto const&, auto const&) {
            ActivateSelected();
        });
        renameButton_ = MakeToolbarButton(L"\xE8AC", CLP_W(CLP_UI_POPUP_RENAME_TIP));
        renameButton_.Click([this](auto const&, auto const&) {
            BeginRenameSelected();
        });
        privateButton_ = MakeToolbarButton(L"\xE72E", CLP_W(CLP_UI_POPUP_MAKE_PRIVATE));
        privateButton_.Click([this](auto const&, auto const&) {
            ToggleSelectedRegisterPrivate();
        });
        deleteButton_ = MakeToolbarButton(L"\xE74D", CLP_W(CLP_UI_POPUP_DELETE_TIP));
        deleteButton_.Click([this](auto const&, auto const&) {
            DeleteSelected();
            FocusFilterBox();
        });
        toolbar.Children().Append(saveButton_);
        toolbar.Children().Append(copyButton_);
        toolbar.Children().Append(renameButton_);
        toolbar.Children().Append(privateButton_);
        toolbar.Children().Append(deleteButton_);
        Grid::SetRow(toolbar, 1);
        root.Children().Append(toolbar);

        // Two star columns: Registers (left, collapsed to zero width until any
        // exist) and the Clipboard stream (right). The column labels appear
        // only when both columns are showing — a lone stream needs no caption.
        Grid columnsGrid;
        regColumnDef_ = ColumnDefinition();
        regColumnDef_.Width(GridLength{ 0, GridUnitType::Pixel });
        ColumnDefinition histColumnDef;
        histColumnDef.Width(GridLength{ 1, GridUnitType::Star });
        columnsGrid.ColumnDefinitions().Append(regColumnDef_);
        columnsGrid.ColumnDefinitions().Append(histColumnDef);

        const auto makeColumnLabel = [](const wchar_t* text) {
            TextBlock label;
            label.Text(winrt::hstring{ text });
            label.FontSize(12);
            label.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            label.Opacity(0.6);
            label.Margin(ThicknessHelper::FromLengths(18, 0, 10, 4));
            return label;
        };

        Grid registerColumn;
        RowDefinition regLabelRow;
        regLabelRow.Height(GridLength{ 0, GridUnitType::Auto });
        RowDefinition regListRow;
        regListRow.Height(GridLength{ 1, GridUnitType::Star });
        registerColumn.RowDefinitions().Append(regLabelRow);
        registerColumn.RowDefinitions().Append(regListRow);
        TextBlock registersLabel = makeColumnLabel(CLP_W(CLP_UI_POPUP_REGISTERS));
        Grid::SetRow(registersLabel, 0);
        registerColumn.Children().Append(registersLabel);
        registerScroll_ = ScrollViewer();
        registerScroll_.Margin(ThicknessHelper::FromLengths(8, 0, 0, 8));
        registerPanel_ = StackPanel();
        registerPanel_.Spacing(2);
        registerScroll_.Content(registerPanel_);
        Grid::SetRow(registerScroll_, 1);
        registerColumn.Children().Append(registerScroll_);
        registerColumnRoot_ = registerColumn;
        registerColumnRoot_.Visibility(Visibility::Collapsed);
        Grid::SetColumn(registerColumn, 0);
        columnsGrid.Children().Append(registerColumn);

        Grid historyColumn;
        RowDefinition histLabelRow;
        histLabelRow.Height(GridLength{ 0, GridUnitType::Auto });
        RowDefinition histListRow;
        histListRow.Height(GridLength{ 1, GridUnitType::Star });
        historyColumn.RowDefinitions().Append(histLabelRow);
        historyColumn.RowDefinitions().Append(histListRow);
        historyLabel_ = makeColumnLabel(CLP_W(CLP_UI_CLIPBOARD));
        historyLabel_.Visibility(Visibility::Collapsed);
        Grid::SetRow(historyLabel_, 0);
        historyColumn.Children().Append(historyLabel_);
        listScroll_ = ScrollViewer();
        listScroll_.Margin(ThicknessHelper::FromLengths(8, 0, 8, 8));
        listPanel_ = StackPanel();
        listPanel_.Spacing(2);
        listScroll_.Content(listPanel_);
        Grid::SetRow(listScroll_, 1);
        historyColumn.Children().Append(listScroll_);
        Grid::SetColumn(historyColumn, 1);
        columnsGrid.Children().Append(historyColumn);

        Grid::SetRow(columnsGrid, 3);
        root.Children().Append(columnsGrid);

        // Second wheel net, this one inside XAML: wherever the island routes
        // the wheel (focused element, pointer target), it bubbles here —
        // handledEventsToo so a consuming control can't hide it.
        root.AddHandler(
            UIElement::PointerWheelChangedEvent(),
            winrt::box_value(Input::PointerEventHandler(
                [this](winrt::Windows::Foundation::IInspectable const&,
                       Input::PointerRoutedEventArgs const& args) {
                    if (!listScroll_) {
                        return;
                    }
                    DismissHintToast();
                    const int delta = args.GetCurrentPoint(nullptr).Properties().MouseWheelDelta();
                    if (delta == 0) {
                        return;
                    }
                    constexpr double kPixelsPerNotch = 96.0;
                    const auto target = WheelTarget();
                    const double offset = target.VerticalOffset()
                        - (static_cast<double>(delta) / WHEEL_DELTA) * kPixelsPerNotch;
                    target.ChangeView(nullptr,
                        winrt::Windows::Foundation::IReference<double>{ offset }, nullptr, true);
                    args.Handled(true);
                })),
            true /* handledEventsToo */);

        // The filter box is the popup's only keyboard home; any click that
        // lands on chrome (header, gaps, scrollbar margins) would otherwise
        // strand XAML focus and kill arrows/Esc. Re-anchor after every click
        // that bubbles this far (rows bubble too — selection still works, and
        // typing keeps flowing).
        root.PointerReleased([this](auto const&, auto const&) {
            DismissHintToast();
            // A click that lands anywhere outside the name editor ends an
            // in-flight rename (the editor swallows its own releases).
            CommitOrCancelRename();
            if (dispatcher_) {
                dispatcher_.TryEnqueue([this]() {
                    if (filterBox_ && hwnd_ != nullptr && IsWindowVisible(hwnd_)) {
                        filterBox_.Focus(FocusState::Programmatic);
                    }
                });
            }
        });

        return root;
    }

    void OnFilterKey(Input::KeyRoutedEventArgs const& args) {
        using winrt::Windows::System::VirtualKey;
        DismissHintToast();  // any keystroke counts as the first action
        const auto key = args.Key();
        const bool filterEmpty = filterBox_ ? filterBox_.Text().empty() : true;

        switch (key) {
        case VirtualKey::Down:
            model_.MoveDown();
            RenderHighlight();
            args.Handled(true);
            return;
        case VirtualKey::Up:
            model_.MoveUp();
            RenderHighlight();
            args.Handled(true);
            return;
        case VirtualKey::Left:
        case VirtualKey::Right:
            // Group hops — but only when the filter box has no text for the
            // caret to move through.
            if (filterEmpty) {
                if (key == VirtualKey::Left) model_.MoveLeft(); else model_.MoveRight();
                RenderHighlight();
                args.Handled(true);
            }
            return;
        case VirtualKey::Enter:
            ActivateSelected();
            args.Handled(true);
            return;
        case VirtualKey::Delete:
            // With filter text present, Delete edits the text; on an empty
            // filter it deletes the selected item everywhere.
            if (filterEmpty) {
                DeleteSelected();
                args.Handled(true);
            }
            return;
        case VirtualKey::F2:
            // Rename the selected register (no-op on history rows).
            BeginRenameSelected();
            args.Handled(true);
            return;
        case VirtualKey::S:
            // Ctrl+S saves the selected clipboard item as a register; a plain
            // 's' keeps flowing into the filter.
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                SaveSelected();
                args.Handled(true);
            }
            return;
        case VirtualKey::Escape: {
            const auto result = model_.HandleEscape();
            if (result == PopupModel::EscapeResult::ClearedFilter) {
                if (filterBox_) {
                    filterBox_.Text(L"");  // TextChanged re-syncs the (already clear) model
                }
            } else if (result == PopupModel::EscapeResult::Close) {
                Dismiss(/*restoreFocus=*/true);
            }
            args.Handled(true);
            return;
        }
        case VirtualKey::Tab:
            args.Handled(true);  // focus stays in the filter box
            return;
        default:
            return;
        }
    }

    // ---- data ----

    // Row-render info for one register, resolved from the record once per
    // rebuild (the store hands out full copies; the popup is short-lived and
    // rebuilds are event-driven, so this is fine).
    struct RegisterRowInfo {
        std::wstring name;         // wide name for render + highlight
        std::wstring previewText;  // content line: text window, kind label, or mask
        std::wstring fullText;     // text registers: the full value, for the flyout
        std::shared_ptr<const std::vector<unsigned char>> imageData;  // image stream, or null
        bool contentRow = false;   // previewText is real content: find matches + re-windows it
        bool isPrivate = false;
        // Age = last touch (reads AND writes), the same clock `clipp ls` shows.
        uint64_t touchedWallMs = 0;
    };

    void RebuildFromStores() {
        registerCache_.clear();
        std::vector<PopupItem> registers;
        auto records = g_registerStore.List();  // live values, name-sorted
        registers.reserve(records.size());
        for (auto& rec : records) {
            if (rec.name.empty()) {
                continue;  // the "" clipboard mirror IS the clipboard column
            }
            RegisterRowInfo info;
            info.name = Utf8ToWideString(rec.name);
            info.isPrivate = rec.IsPrivate();
            info.touchedWallMs = rec.touched.wallMs;

            PopupItem item;
            item.kind = PopupItem::Kind::Register;
            item.registerName = rec.name;
            // Unlike history kind-labels, register NAMES are user data — the
            // primary handle — so they participate in find (and light up).
            item.searchText = info.name;

            if (rec.IsPrivate()) {
                info.previewText = L"••••••••";  // fixed width: not length-revealing
            } else if (rec.IsBinary()) {
                RegisterWire::BinaryValueInfo bin{};
                if (RegisterWire::TryParseBinaryValue(rec.value, bin)
                    && IsClippImageFormat(bin.formatId)) {
                    info.previewText = CLP_W(CLP_UI_IMAGE);
                    info.imageData = std::make_shared<const std::vector<unsigned char>>(
                        rec.value.begin() + static_cast<std::ptrdiff_t>(bin.streamOffset),
                        rec.value.end());
                } else {
                    info.previewText = CLP_W(CLP_UI_UNSUPPORTED_CLIPBOARD_ITEM);
                }
            } else {
                info.contentRow = true;
                info.fullText = Utf8ToWideString(rec.value);
                info.previewText = info.fullText.size() > kRegisterPreviewChars
                    ? info.fullText.substr(0, kRegisterPreviewChars) + L"..."
                    : info.fullText;
                item.searchText += L"\n" + info.previewText;
            }
            registerCache_.emplace(rec.name, std::move(info));
            registers.push_back(std::move(item));
        }
        registersPresent_ = !registers.empty();

        displayCache_.clear();
        std::vector<PopupItem> history;
        const auto snapshot = g_clipboardActivityStore.Snapshot();  // ascending by ts
        history.reserve(snapshot.size());
        for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
            auto display = g_clipboardActivityStore.DisplayItem(it->id);
            if (!display) {
                continue;
            }
            PopupItem item;
            item.kind = PopupItem::Kind::History;
            item.historyId = it->id;
            // Type-to-find matches CONTENT only — kind labels, device names,
            // and ages are neither located nor highlighted. Non-text rows
            // simply drop out of a filtered view.
            const bool contentKind =
                display->kind == ClipboardActivityPayloadKind::Text ||
                display->kind == ClipboardActivityPayloadKind::Link;
            item.searchText = contentKind ? display->previewText : std::wstring{};
            item.actionable = display->kind != ClipboardActivityPayloadKind::PrivatePlaceholder;
            displayCache_.emplace(it->id, std::move(*display));
            history.push_back(std::move(item));
        }
        model_.SetItems(std::move(registers), std::move(history));
    }

    // Registers have no store watcher (their remote traffic is rare and the
    // activity watcher's rebuild re-reads them anyway); popup-initiated ops
    // call this to refresh explicitly, optionally re-selecting one by name.
    void RefreshAfterRegisterOp(const std::optional<std::string>& selectName) {
        RebuildFromStores();
        UpdateColumnLayout();
        if (selectName.has_value()) {
            const auto& regs = model_.VisibleRegisters();
            for (std::size_t i = 0; i < regs.size(); ++i) {
                if (regs[i]->registerName == *selectName) {
                    model_.SelectAt(PopupModel::Group::Registers, i);
                    break;
                }
            }
        }
        RenderList();
    }

    // ---- rendering ----

    void RenderList() {
        if (!listPanel_ || !registerPanel_) {
            return;
        }
        listPanel_.Children().Clear();
        registerPanel_.Children().Clear();
        rowBorders_.clear();
        registerRowBorders_.clear();
        nameEditor_ = nullptr;  // re-created below while a rename is in flight

        const auto makeMoreHint = []() {
            TextBlock more;
            more.Text(winrt::hstring{ CLP_W(CLP_UI_POPUP_MORE) });
            more.Opacity(0.55);
            more.FontSize(12);
            more.Margin(ThicknessHelper::FromLengths(10, 6, 10, 6));
            return more;
        };

        const auto& registers = model_.VisibleRegisters();
        const std::size_t shownRegisters = (std::min)(registers.size(), kMaxRenderedRows);
        for (std::size_t i = 0; i < shownRegisters; ++i) {
            registerPanel_.Children().Append(BuildRegisterRow(*registers[i], i));
        }
        if (registers.size() > shownRegisters) {
            registerPanel_.Children().Append(makeMoreHint());
        }
        // No empty-state text for registers: with none at all the whole column
        // is collapsed, and a filtered-empty column reads fine bare.

        const auto& history = model_.VisibleHistory();
        const std::size_t shown = (std::min)(history.size(), kMaxRenderedRows);
        for (std::size_t i = 0; i < shown; ++i) {
            listPanel_.Children().Append(BuildRow(*history[i], i));
        }
        if (history.size() > shown) {
            listPanel_.Children().Append(makeMoreHint());
        }
        if (history.empty()) {
            // Covers both "nothing synced yet" and "filter matched nothing".
            TextBlock empty;
            empty.Text(winrt::hstring{ CLP_W(CLP_UI_POPUP_EMPTY) });
            empty.Opacity(0.55);
            empty.Margin(ThicknessHelper::FromLengths(10, 16, 10, 6));
            empty.TextWrapping(TextWrapping::Wrap);
            empty.HorizontalAlignment(HorizontalAlignment::Center);
            listPanel_.Children().Append(empty);
        }
        RenderHighlight();
    }

    Border BuildRow(const PopupItem& item, std::size_t index) {
        const auto cached = displayCache_.find(item.historyId);

        StackPanel content;
        content.Spacing(1);

        TextBlock preview;
        std::wstring previewText;
        std::wstring metaText;
        bool contentRow = false;  // true when previewText is CONTENT, not a label
        if (cached != displayCache_.end()) {
            const auto& display = cached->second;
            switch (display.kind) {
            case ClipboardActivityPayloadKind::Image:
                previewText = CLP_W(CLP_UI_IMAGE);
                break;
            case ClipboardActivityPayloadKind::PrivatePlaceholder:
                previewText = CLP_W(CLP_UI_PRIVATE_PLACEHOLDER_TITLE);
                break;
            default:
                previewText = display.previewText;
                break;
            }
            contentRow = display.kind == ClipboardActivityPayloadKind::Text ||
                         display.kind == ClipboardActivityPayloadKind::Link;
            // "Mars11 14 seconds ago" — who it came from, and how fresh.
            metaText = display.deviceName.empty()
                ? RelativeAgeText(display.header.timestamp)
                : display.deviceName + L" " + RelativeAgeText(display.header.timestamp);
        }
        if (previewText.empty()) {
            previewText = L" ";
        }
        // Find applies to content only — labels and meta lines are exempt from
        // both matching and highlighting. A content match past the single-line
        // ellipsis would be invisible, so re-window the text around the first
        // match instead of scrolling anything.
        if (contentRow && !model_.Filter().empty()) {
            const auto matches = FindMatches(previewText, model_.Filter());
            if (!matches.empty() && matches.front() > kRowMatchLeadChars) {
                previewText = L"…" + previewText.substr(matches.front() - kRowMatchLeadChars);
            }
        }
        preview.Text(winrt::hstring{ previewText });
        preview.TextTrimming(TextTrimming::CharacterEllipsis);
        preview.TextWrapping(TextWrapping::NoWrap);
        preview.MaxLines(1);
        if (contentRow) {
            HighlightMatches(preview, previewText, model_.Filter());
        }
        content.Children().Append(preview);

        if (!metaText.empty()) {
            TextBlock meta;
            meta.Text(winrt::hstring{ metaText });
            meta.FontSize(11);
            meta.Opacity(0.6);
            content.Children().Append(meta);
        }

        Border row;
        row.Padding(ThicknessHelper::FromLengths(10, 6, 10, 6));
        row.CornerRadius(CornerRadius{ 6 });
        row.Background(ArgbBrush(0, 0, 0, 0));
        row.Child(content);

        row.PointerPressed([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            RenderHighlight();
        });
        row.DoubleTapped([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            ActivateSelected();
        });

        MenuFlyout menu;
        MenuFlyoutItem copyItem;
        copyItem.Text(winrt::hstring{ CLP_W(CLP_UI_COPY) });
        copyItem.Click([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            ActivateSelected();
        });
        menu.Items().Append(copyItem);
        MenuFlyoutItem deleteItem;
        deleteItem.Text(winrt::hstring{ CLP_W(CLP_UI_DELETE) });
        deleteItem.Click([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            DeleteSelected();
        });
        menu.Items().Append(deleteItem);
        row.ContextFlyout(menu);

        rowBorders_.push_back(row);
        return row;
    }

    Border BuildRegisterRow(const PopupItem& item, std::size_t index) {
        const auto cached = registerCache_.find(item.registerName);

        StackPanel content;
        content.Spacing(1);

        // Name line — or, mid-rename, the inline editor in its place.
        const std::wstring nameText =
            cached != registerCache_.end() ? cached->second.name : std::wstring{ L" " };
        const bool editing =
            editingRegister_.has_value() && *editingRegister_ == item.registerName;
        if (editing) {
            nameEditor_ = TextBox();
            nameEditor_.Text(winrt::hstring{ nameText });
            nameEditor_.FontSize(13);
            nameEditor_.MinHeight(0);
            nameEditor_.Padding(ThicknessHelper::FromLengths(4, 2, 4, 2));
            nameEditor_.Loaded([](auto const& sender, auto const&) {
                // The editor is born with the row render; grab the keyboard
                // and preselect the auto-name so typing replaces it.
                if (const auto box = sender.template try_as<TextBox>()) {
                    box.Focus(FocusState::Programmatic);
                    box.SelectAll();
                }
            });
            nameEditor_.TextChanged([this](auto const&, auto const&) {
                ValidateNameEditor();
            });
            nameEditor_.PreviewKeyDown([this](auto const&, Input::KeyRoutedEventArgs const& args) {
                using winrt::Windows::System::VirtualKey;
                switch (args.Key()) {
                case VirtualKey::Enter:
                    CommitRename(/*keepSelection=*/true);
                    args.Handled(true);
                    return;
                case VirtualKey::Escape:
                    CancelRename();
                    args.Handled(true);
                    return;
                case VirtualKey::Tab:
                    args.Handled(true);
                    return;
                default:
                    return;
                }
            });
            // Caret clicks stay in the editor — they must not reach the
            // root's click-away commit.
            nameEditor_.PointerReleased([](auto const&, Input::PointerRoutedEventArgs const& args) {
                args.Handled(true);
            });
            content.Children().Append(nameEditor_);
        } else {
            TextBlock name;
            name.Text(winrt::hstring{ nameText });
            name.FontSize(13);
            name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            name.TextTrimming(TextTrimming::CharacterEllipsis);
            name.TextWrapping(TextWrapping::NoWrap);
            name.MaxLines(1);
            HighlightMatches(name, nameText, model_.Filter());
            content.Children().Append(name);
        }

        // Content line: same content-only find contract as history rows.
        TextBlock preview;
        std::wstring previewText =
            cached != registerCache_.end() ? cached->second.previewText : std::wstring{ L" " };
        if (previewText.empty()) {
            previewText = L" ";
        }
        const bool contentRow = cached != registerCache_.end() && cached->second.contentRow;
        if (contentRow && !model_.Filter().empty()) {
            const auto matches = FindMatches(previewText, model_.Filter());
            if (!matches.empty() && matches.front() > kRowMatchLeadChars) {
                previewText = L"…" + previewText.substr(matches.front() - kRowMatchLeadChars);
            }
        }
        preview.Text(winrt::hstring{ previewText });
        preview.TextTrimming(TextTrimming::CharacterEllipsis);
        preview.TextWrapping(TextWrapping::NoWrap);
        preview.MaxLines(1);
        preview.Opacity(0.75);  // the name is the row's headline
        if (contentRow) {
            HighlightMatches(preview, previewText, model_.Filter());
        }
        content.Children().Append(preview);

        if (cached != registerCache_.end()) {
            TextBlock meta;
            std::wstring metaText = RelativeAgeText(cached->second.touchedWallMs);
            if (cached->second.isPrivate) {
                metaText += L" · " CLP_W(CLP_UI_PRIVATE_BADGE);
            }
            meta.Text(winrt::hstring{ metaText });
            meta.FontSize(11);
            meta.Opacity(0.6);
            content.Children().Append(meta);
        }

        Border row;
        row.Padding(ThicknessHelper::FromLengths(10, 6, 10, 6));
        row.CornerRadius(CornerRadius{ 6 });
        row.Background(ArgbBrush(0, 0, 0, 0));
        row.Child(content);

        row.PointerPressed([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::Registers, index);
            RenderHighlight();
        });
        row.DoubleTapped([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::Registers, index);
            ActivateSelected();
        });

        MenuFlyout menu;
        MenuFlyoutItem copyItem;
        copyItem.Text(winrt::hstring{ CLP_W(CLP_UI_COPY) });
        copyItem.Click([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::Registers, index);
            ActivateSelected();
        });
        menu.Items().Append(copyItem);
        MenuFlyoutItem renameItem;
        renameItem.Text(winrt::hstring{ CLP_W(CLP_UI_POPUP_RENAME) });
        renameItem.Click([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::Registers, index);
            BeginRenameSelected();
        });
        menu.Items().Append(renameItem);
        MenuFlyoutItem privateItem;
        privateItem.Text(winrt::hstring{
            cached != registerCache_.end() && cached->second.isPrivate
                ? CLP_W(CLP_UI_POPUP_MAKE_PUBLIC) : CLP_W(CLP_UI_POPUP_MAKE_PRIVATE) });
        privateItem.Click([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::Registers, index);
            ToggleSelectedRegisterPrivate();
        });
        menu.Items().Append(privateItem);
        MenuFlyoutItem deleteItem;
        deleteItem.Text(winrt::hstring{ CLP_W(CLP_UI_DELETE) });
        deleteItem.Click([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::Registers, index);
            DeleteSelected();
        });
        menu.Items().Append(deleteItem);
        row.ContextFlyout(menu);

        registerRowBorders_.push_back(row);
        return row;
    }

    void RenderHighlight() {
        const auto selection = model_.Selected();
        const auto paint = [&selection](std::vector<Border>& borders, PopupModel::Group group) {
            for (std::size_t i = 0; i < borders.size(); ++i) {
                const bool selected = selection.has_value()
                    && selection->group == group
                    && selection->index == i;
                borders[i].Background(selected ? ArgbBrush(56, 127, 127, 127)
                                               : ArgbBrush(0, 0, 0, 0));
                if (selected) {
                    borders[i].StartBringIntoView();
                }
            }
        };
        paint(registerRowBorders_, PopupModel::Group::Registers);
        paint(rowBorders_, PopupModel::Group::History);
        UpdateToolbar();
        SchedulePreviewFlyoutUpdate();
    }

    // The flyout anchors to the selected row's on-screen position, which for
    // freshly built rows only exists after the next layout pass — defer one
    // dispatcher hop (coalesced) instead of measuring mid-render.
    void SchedulePreviewFlyoutUpdate() {
        if (!dispatcher_) {
            UpdatePreviewFlyout();
            return;
        }
        if (previewUpdatePending_) {
            return;
        }
        previewUpdatePending_ = true;
        dispatcher_.TryEnqueue([this]() {
            previewUpdatePending_ = false;
            UpdatePreviewFlyout();
        });
    }

    void UpdateToolbar() {
        const PopupItem* item = model_.SelectedItem();
        bool canSave = false;
        if (item != nullptr && item->kind == PopupItem::Kind::History && item->actionable) {
            const auto cached = displayCache_.find(item->historyId);
            if (cached != displayCache_.end()) {
                const auto kind = cached->second.kind;
                canSave = kind == ClipboardActivityPayloadKind::Text
                    || kind == ClipboardActivityPayloadKind::Link
                    || kind == ClipboardActivityPayloadKind::Image
                    || kind == ClipboardActivityPayloadKind::PrivateText;
            }
        }
        const bool registerSelected =
            item != nullptr && item->kind == PopupItem::Kind::Register;
        bool selectedPrivate = false;
        if (registerSelected) {
            const auto cached = registerCache_.find(item->registerName);
            selectedPrivate = cached != registerCache_.end() && cached->second.isPrivate;
        }
        if (saveButton_) {
            saveButton_.IsEnabled(canSave);
        }
        if (copyButton_) {
            copyButton_.IsEnabled(item != nullptr && item->actionable);
        }
        if (renameButton_) {
            renameButton_.IsEnabled(registerSelected);
        }
        if (privateButton_) {
            privateButton_.IsEnabled(registerSelected);
            // The button shows the ACTION: lock a public register, unlock a
            // private one.
            if (const auto icon = privateButton_.Content().try_as<FontIcon>()) {
                icon.Glyph(selectedPrivate ? L"\xE785" : L"\xE72E");
            }
            ToolTipService::SetToolTip(privateButton_, winrt::box_value(winrt::hstring{
                selectedPrivate ? CLP_W(CLP_UI_POPUP_MAKE_PUBLIC)
                                : CLP_W(CLP_UI_POPUP_MAKE_PRIVATE) }));
        }
        if (deleteButton_) {
            deleteButton_.IsEnabled(item != nullptr);
        }
    }

    // Screen Y of the selected row's top edge — the flyout's anchor. The
    // island fills the borderless popup's client area at (0,0), so island
    // dips map straight onto the window origin.
    int RowAnchorScreenY() {
        RECT popupRect{};
        GetWindowRect(hwnd_, &popupRect);
        const auto selection = model_.Selected();
        if (selection.has_value()) {
            const auto& borders = selection->group == PopupModel::Group::Registers
                ? registerRowBorders_ : rowBorders_;
            if (selection->index < borders.size()) {
                try {
                    auto point = borders[selection->index].TransformToVisual(nullptr)
                        .TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
                    // A freshly built row that hasn't been arranged yet
                    // transforms to ~0 — a Y no real row can have (header,
                    // toolbar and filter sit above them all). A dispatcher
                    // hop is NOT reliably after the arrange pass, so force
                    // one (the ClippPage FLIP animation's proven pattern)
                    // and ask again; only then fall back.
                    if (point.Y < 1.0f) {
                        borders[selection->index].UpdateLayout();
                        point = borders[selection->index].TransformToVisual(nullptr)
                            .TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
                    }
                    if (point.Y >= 1.0f) {
                        return popupRect.top + DipsToPixels(point.Y, GetDpiForWindow(hwnd_));
                    }
                } catch (const winrt::hresult_error&) {
                }
            }
        }
        return popupRect.top + DipsToPixels(80, GetDpiForWindow(hwnd_));
    }

    // Text flyout body: the REGION around the first filter match — visible by
    // construction, nothing to scroll.
    void ShowTextFlyoutWindowed(const std::wstring& full, bool preferLeft) {
        const std::wstring filter = filterBox_ ? std::wstring{ filterBox_.Text() } : std::wstring{};
        std::size_t firstMatch = std::wstring::npos;
        if (!filter.empty()) {
            const auto matches = FindMatches(full, filter);
            if (!matches.empty()) {
                firstMatch = matches.front();
            }
        }
        std::size_t begin = 0;
        if (firstMatch != std::wstring::npos && firstMatch > kPreviewLeadChars) {
            begin = firstMatch - kPreviewLeadChars;
        }
        std::wstring shown = full.substr(begin, kPreviewWindowChars);
        const bool clippedFront = begin > 0;
        const bool clippedBack = begin + shown.size() < full.size();
        if (clippedFront) {
            shown.insert(0, L"… ");
        }
        if (clippedBack) {
            shown.append(L" …");
        }
        previewWindow_.ShowText(hwnd_, RowAnchorScreenY(), shown, filter, preferLeft);
    }

    // The flyout appears only when the selection holds more than its row can
    // show: an image, or long/multiline text. Masked private rows, the
    // placeholder, and unsupported items add nothing and get none. Register
    // rows open it on the popup's LEFT — their own column's side.
    void UpdatePreviewFlyout() {
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr || hwnd_ == nullptr || !IsWindowVisible(hwnd_)
            || editingRegister_.has_value()) {
            previewWindow_.Hide();  // no selection, hidden popup, or mid-rename
            return;
        }

        if (item->kind == PopupItem::Kind::Register) {
            const auto cached = registerCache_.find(item->registerName);
            if (cached == registerCache_.end()) {
                previewWindow_.Hide();
                return;
            }
            const auto& info = cached->second;
            if (info.imageData) {
                previewWindow_.ShowImage(hwnd_, RowAnchorScreenY(), info.imageData, true);
                return;
            }
            if (!info.contentRow) {
                previewWindow_.Hide();  // masked private / unsupported binary
                return;
            }
            const std::wstring& full = info.fullText;
            if (TextFitsInRow(full)) {
                previewWindow_.Hide();  // the row already tells the whole story
                return;
            }
            ShowTextFlyoutWindowed(full, /*preferLeft=*/true);
            return;
        }

        const auto cached = displayCache_.find(item->historyId);
        if (cached == displayCache_.end()) {
            previewWindow_.Hide();
            return;
        }

        const auto& display = cached->second;
        // Content only — the row already carries the who/when labels.
        if (display.kind == ClipboardActivityPayloadKind::Image && display.imageData) {
            previewWindow_.ShowImage(hwnd_, RowAnchorScreenY(), display.imageData, false);
            return;
        }

        if (display.kind != ClipboardActivityPayloadKind::Text &&
            display.kind != ClipboardActivityPayloadKind::Link) {
            previewWindow_.Hide();
            return;
        }

        const std::wstring& full =
            display.detailText.empty() ? display.previewText : display.detailText;
        if (TextFitsInRow(full)) {
            previewWindow_.Hide();  // the row already tells the whole story
            return;
        }
        ShowTextFlyoutWindowed(full, /*preferLeft=*/false);
    }

    void DismissHintToast() {
        toastWindow_.Hide();
    }

    // The scroll viewer under the mouse. Both wheel nets ask HERE rather than
    // trusting message/event coordinates (island pointer positions proved
    // unreliable): the live cursor position against the window midline — the
    // two columns are equal stars, so the seam IS the midline.
    ScrollViewer WheelTarget() {
        if (registersPresent_ && registerScroll_ && hwnd_ != nullptr) {
            POINT cursor{};
            RECT rect{};
            if (GetCursorPos(&cursor) && GetWindowRect(hwnd_, &rect)
                && cursor.x < (rect.left + rect.right) / 2) {
                return registerScroll_;
            }
        }
        return listScroll_;
    }

    // ---- actions ----

    void ActivateSelected() {
        DismissHintToast();      // button-borne invocations skip the root handler
        CommitOrCancelRename();  // an action supersedes an in-flight rename
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr || !item->actionable) {
            return;
        }
        if (item->kind == PopupItem::Kind::History) {
            clipp::ReshareActivityItem(item->historyId);
        } else {
            clipp::MakeRegisterCurrent(item->registerName);
        }
        Dismiss(/*restoreFocus=*/true);
    }

    void DeleteSelected() {
        DismissHintToast();
        CommitOrCancelRename();
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr) {
            return;
        }
        if (item->kind == PopupItem::Kind::History) {
            clipp::DeleteActivityItemEverywhere(item->historyId);
            // The watcher event rebuilds the list.
        } else {
            clipp::DeleteRegisterEverywhere(item->registerName);
            RefreshAfterRegisterOp(std::nullopt);  // no register watcher: explicit
        }
    }

    // "Save": promote the selected clipboard item into an auto-named register
    // and drop straight into naming it. Masked content — whether the source
    // marked it or our own heuristic did — carries PRIVATE onto the register.
    void SaveSelected() {
        DismissHintToast();
        CommitOrCancelRename();
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr || item->kind != PopupItem::Kind::History || !item->actionable) {
            return;
        }
        const auto cached = displayCache_.find(item->historyId);
        if (cached == displayCache_.end()) {
            return;
        }
        const auto kind = cached->second.kind;
        const bool saveable = kind == ClipboardActivityPayloadKind::Text
            || kind == ClipboardActivityPayloadKind::Link
            || kind == ClipboardActivityPayloadKind::Image
            || kind == ClipboardActivityPayloadKind::PrivateText;
        if (!saveable) {
            return;
        }
        const bool markPrivate = cached->second.sourceMarked
            || kind == ClipboardActivityPayloadKind::PrivateText;

        const std::string name = NextAutoRegisterName(g_registerStore.ListNames());
        if (!clipp::SaveActivityItemAsRegister(item->historyId, name, markPrivate)) {
            return;
        }

        // Pivot the popup to the result: the new row must be visible (clear
        // any filter), selected, and immediately renameable.
        if (filterBox_ && !filterBox_.Text().empty()) {
            filterBox_.Text(L"");  // TextChanged clears the model filter + re-renders
        }
        editingRegister_ = name;
        model_.EnterEditMode();
        RefreshAfterRegisterOp(name);  // renders the new row as the inline editor
    }

    // Flip the selected register's PRIVATE flag mesh-wide.
    void ToggleSelectedRegisterPrivate() {
        DismissHintToast();
        CommitOrCancelRename();
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr || item->kind != PopupItem::Kind::Register) {
            return;
        }
        const std::string name = item->registerName;  // survives the refresh below
        const auto cached = registerCache_.find(name);
        const bool isPrivate = cached != registerCache_.end() && cached->second.isPrivate;
        if (clipp::SetRegisterPrivate(name, !isPrivate)) {
            RefreshAfterRegisterOp(name);
            FocusFilterBox();
        }
    }

    // ---- inline rename ----

    void BeginRenameSelected() {
        DismissHintToast();
        if (editingRegister_.has_value()) {
            return;  // already editing
        }
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr || item->kind != PopupItem::Kind::Register) {
            return;
        }
        editingRegister_ = item->registerName;
        model_.EnterEditMode();
        RenderList();  // the selected row re-renders as the editor; Loaded focuses it
    }

    // The normalized/trimmed editor text, if it names a legal rename target:
    // passes the shared validator and collides with nothing (other than the
    // register being renamed).
    bool NameEditorTarget(std::string& outName) {
        if (!nameEditor_ || !editingRegister_.has_value()) {
            return false;
        }
        std::string name = WideToUtf8String(std::wstring{ nameEditor_.Text() });
        const std::size_t first = name.find_first_not_of(' ');
        if (first == std::string::npos) {
            outName.clear();
            return false;
        }
        const std::size_t last = name.find_last_not_of(' ');
        name = name.substr(first, last - first + 1);
        name = clipp_platform_detail::NormalizeUtf8Canonical(name);
        outName = name;
        if (!IsValidRegisterName(name)) {
            return false;
        }
        if (name != *editingRegister_ && registerCache_.count(name) > 0) {
            return false;  // would silently overwrite a sibling
        }
        return true;
    }

    void ValidateNameEditor() {
        if (!nameEditor_) {
            return;
        }
        std::string ignored;
        if (NameEditorTarget(ignored)) {
            nameEditor_.ClearValue(Control::BorderBrushProperty());
        } else {
            nameEditor_.BorderBrush(ArgbBrush(255, 200, 60, 60));
        }
    }

    void CommitRename(bool keepSelection) {
        if (!editingRegister_.has_value()) {
            return;
        }
        std::string newName;
        if (!NameEditorTarget(newName)) {
            ValidateNameEditor();  // stay in the editor, painted invalid
            return;
        }
        const std::string oldName = *editingRegister_;
        EndEditMode();
        if (newName != oldName) {
            clipp::RenameRegister(oldName, newName);
        }
        RefreshAfterRegisterOp(keepSelection ? std::optional<std::string>(newName)
                                             : std::nullopt);
        FocusFilterBox();
    }

    void CancelRename() {
        if (!editingRegister_.has_value()) {
            return;
        }
        EndEditMode();
        RefreshAfterRegisterOp(std::nullopt);
        FocusFilterBox();
    }

    // Click-away and action-supersede: commit if the editor holds a valid
    // name, abandon if not — never trap the user in a red box.
    void CommitOrCancelRename() {
        if (!editingRegister_.has_value()) {
            return;
        }
        std::string newName;
        if (NameEditorTarget(newName)) {
            CommitRename(/*keepSelection=*/false);
        } else {
            CancelRename();
        }
    }

    void EndEditMode() {
        editingRegister_.reset();
        nameEditor_ = nullptr;
        model_.LeaveEditMode();
    }

    // ---- store watcher (visible only) ----

    void BeginActivityNotifications() {
        if (watcherID_ != 0) {
            return;
        }
        if (!dispatcher_) {
            dispatcher_ = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
        }
        const auto registration = g_clipboardActivityStore.QueryAndRegister(&PopupWindow::ActivityWatcher, this);
        watcherID_ = registration.watcherID;
    }

    void EndActivityNotifications() {
        if (watcherID_ == 0) {
            return;
        }
        g_clipboardActivityStore.Unregister(watcherID_);
        watcherID_ = 0;
    }

    static void ActivityWatcher(const ClipboardActivityUpdate&, void* userData) {
        auto* self = static_cast<PopupWindow*>(userData);
        if (self == nullptr || !self->dispatcher_) {
            return;
        }
        // Coarse but correct: any store change re-snapshots while visible.
        // Except mid-rename — a re-render would rebuild the editor row and
        // eat the user's typing; the commit/cancel path refreshes instead.
        self->dispatcher_.TryEnqueue([self]() {
            if (self->hwnd_ != nullptr && IsWindowVisible(self->hwnd_)
                && !self->editingRegister_.has_value()) {
                self->RebuildFromStores();
                self->UpdateColumnLayout();
                self->RenderList();
            }
        });
    }

    // ---- window plumbing ----

    // One column of clipboard stream, or two once any registers exist.
    double CurrentWidthDips() const {
        return registersPresent_ ? kPopupWidthTwoColDips : kPopupWidthDips;
    }

    // Reflect registersPresent_ into the XAML tree (column width, labels) and
    // — when the flip happens mid-session (first save / last delete while
    // open) — into the window width, keeping the popup centered where it was.
    void UpdateColumnLayout() {
        if (regColumnDef_) {
            regColumnDef_.Width(registersPresent_
                ? GridLength{ 1, GridUnitType::Star }
                : GridLength{ 0, GridUnitType::Pixel });
        }
        const auto vis = registersPresent_ ? Visibility::Visible : Visibility::Collapsed;
        if (registerColumnRoot_) {
            registerColumnRoot_.Visibility(vis);
        }
        if (historyLabel_) {
            historyLabel_.Visibility(vis);
        }
        if (hwnd_ == nullptr || !IsWindowVisible(hwnd_)) {
            return;  // hidden: the next PositionOnCursorMonitor sizes it
        }
        RECT rect{};
        GetWindowRect(hwnd_, &rect);
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int width = DipsToPixels(CurrentWidthDips(), dpi);
        if (width == rect.right - rect.left) {
            return;
        }
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info);
        int x = (rect.left + rect.right - width) / 2;
        if (x + width > info.rcWork.right) {
            x = info.rcWork.right - width;
        }
        if (x < info.rcWork.left) {
            x = info.rcWork.left;
        }
        SetWindowPos(hwnd_, HWND_TOPMOST, x, rect.top, width, rect.bottom - rect.top,
            SWP_NOACTIVATE);
        ResizeXamlHost();
    }

    void PositionOnCursorMonitor() {
        POINT cursor{};
        GetCursorPos(&cursor);
        const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return;
        }
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int width = DipsToPixels(CurrentWidthDips(), dpi);
        const int height = DipsToPixels(kPopupHeightDips, dpi);
        const RECT& work = info.rcWork;
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
        ResizeXamlHost();
    }

    void ResizeXamlHost() {
        if (xamlHost_ == nullptr || hwnd_ == nullptr) {
            return;
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        SetWindowPos(xamlHost_, nullptr, 0, 0,
            client.right - client.left, client.bottom - client.top,
            SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                // Light dismiss — unless focus went to a window of our own
                // thread (the island's flyout popups live in sibling HWNDs).
                const HWND other = reinterpret_cast<HWND>(lParam);
                const DWORD ourThread = GetCurrentThreadId();
                if (other == nullptr ||
                    GetWindowThreadProcessId(other, nullptr) != ourThread) {
                    Dismiss(/*restoreFocus=*/false);
                }
            }
            return 0;
        case WM_SIZE:
            ResizeXamlHost();
            return 0;
        case WM_SETFOCUS:
            // Win32 focus on the top-level must flow into the island — and all
            // the way back to the filter box, or the keyboard lands nowhere.
            FocusFilterBox();
            return 0;
        case WM_DPICHANGED: {
            // The satellites' geometry is stale at the new DPI; they re-derive
            // it on their next show.
            toastWindow_.Hide();
            previewWindow_.Hide();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            Dismiss(/*restoreFocus=*/false);
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
    }

    static void RegisterPopupClass() {
        static bool registered = false;
        if (registered) {
            return;
        }
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPopupClassName;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        PopupWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<PopupWindow*>(createStruct->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<PopupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self == nullptr) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        const LRESULT result = self->HandleMessage(msg, wParam, lParam);
        if (msg == WM_NCDESTROY) {
            self->hwnd_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return result;
    }

    HWND hwnd_ = nullptr;
    HWND xamlHost_ = nullptr;
    HWND previousForeground_ = nullptr;
    Hosting::WindowsXamlManager xamlManager_{ nullptr };
    Hosting::DesktopWindowXamlSource xamlSource_{ nullptr };
    winrt::Windows::System::DispatcherQueue dispatcher_{ nullptr };
    TextBox filterBox_{ nullptr };
    TextBlock filterHint_{ nullptr };
    ScrollViewer listScroll_{ nullptr };
    StackPanel listPanel_{ nullptr };
    ScrollViewer registerScroll_{ nullptr };
    StackPanel registerPanel_{ nullptr };
    Grid registerColumnRoot_{ nullptr };
    TextBlock historyLabel_{ nullptr };
    ColumnDefinition regColumnDef_{ nullptr };
    Button saveButton_{ nullptr };
    Button copyButton_{ nullptr };
    Button renameButton_{ nullptr };
    Button privateButton_{ nullptr };
    Button deleteButton_{ nullptr };
    TextBox nameEditor_{ nullptr };
    bool previewUpdatePending_ = false;
    ToastWindow toastWindow_;
    PreviewWindow previewWindow_;
    std::vector<Border> rowBorders_;
    std::vector<Border> registerRowBorders_;
    std::unordered_map<uint64_t, ClipboardActivityDisplayItem> displayCache_;
    std::map<std::string, RegisterRowInfo> registerCache_;
    std::optional<std::string> editingRegister_;
    bool registersPresent_ = false;
    PopupModel model_;
    std::size_t watcherID_ = 0;
};

std::unique_ptr<PopupWindow> g_popupWindow;

}  // namespace

namespace clipp {

void TogglePopupWindow() {
    if (!g_popupWindow) {
        g_popupWindow = std::make_unique<PopupWindow>();
    }
    g_popupWindow->Toggle();
}

bool PopupPreTranslateMessage(MSG* msg) {
    return g_popupWindow ? g_popupWindow->PreTranslateMessage(msg) : false;
}

void DestroyPopupWindow() {
    if (g_popupWindow) {
        g_popupWindow->Destroy();
        g_popupWindow.reset();
    }
}

}  // namespace clipp
