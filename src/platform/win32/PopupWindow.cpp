#include "platform.h"

#include "PopupWindow.h"

#include "ClipboardActions.h"
#include "ClipboardActivityStore.h"
#include "ClipboardFormat.h"
#include "Logger.h"
#include "PopupModel.h"
#include "PopupTextMatch.h"
#include "RegisterStore.h"
#include "RegisterWire.h"
#include "Settings.h"
#include "TextTyper.h"
#include "TypeLayout.h"
#include "TypePlan.h"
#include "clipp-win32-darkmode32/DMSubclass.h"
#include "platform/uiClippPage.h"
#include "platform/uistrings.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <set>
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
// Preview flyout geometry: content-sized, up to these caps.
constexpr double kPreviewMaxTextWidthDips = 360;
constexpr double kPreviewMaxHeightDips = 520;

// The find / re-window / fits-in-a-row text rules are shared with the macOS
// shell (PopupTextMatch.h) so both popups match and window identically.
using popupfind::kMaxRenderedRows;
using popupfind::kRegisterPreviewChars;
using popupfind::kRowFitChars;
using popupfind::kMaxHighlightRanges;
using popupfind::FindMatches;
using popupfind::TextFitsInRow;

int DipsToPixels(double dips, UINT dpi) {
    return static_cast<int>(std::ceil(dips * dpi / USER_DEFAULT_SCREEN_DPI));
}

ElementTheme CurrentTheme() {
    return DarkMode::isEnabled() ? ElementTheme::Dark : ElementTheme::Light;
}

// "hwnd=0x0012ABCD class='CASCADIA_HOSTING_WINDOW_CLASS' title='pwsh' pid=1234"
// — enough to tell OUR windows from the real paste target in a log.
std::wstring DescribeWindow(HWND hwnd) {
    if (hwnd == nullptr) {
        return L"hwnd=null";
    }
    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    wchar_t title[128] = {};
    GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t buffer[512];
    _snwprintf_s(buffer, _TRUNCATE,
        L"hwnd=0x%p class='%s' title='%s' pid=%lu visible=%d",
        static_cast<void*>(hwnd), className, title, static_cast<unsigned long>(pid),
        IsWindowVisible(hwnd) ? 1 : 0);
    return buffer;
}

// The raw OS preference, read independently of darkmode32's cached state so a
// log line can show BOTH (they disagreeing is itself the diagnosis).
int AppsUseLightThemeRegValue() {
    DWORD data = 0;
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) {
        return -1;  // absent — Windows treats that as light
    }
    return static_cast<int>(data);
}

SolidColorBrush ArgbBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b));
}

// Row previews are a single clipped line, so leading blank lines (or indent)
// would push the real content out of sight and leave the row looking empty.
// Display-only: every action still works on the untrimmed value.
std::wstring TrimLeadingWhitespace(const std::wstring& text) {
    std::size_t first = 0;
    while (first < text.size() && std::iswspace(static_cast<wint_t>(text[first])) != 0) {
        ++first;
    }
    return first == 0 ? text : text.substr(first);
}

// ---- palette ---------------------------------------------------------------
// The popup lives in ClippPage's visual family, warmed up: the page's world is
// accent-blue washes (outgoing bubbles), amber accents (private badges, the
// find highlighter), and soft translucency — the popup's original flat grays
// read utilitarian next to it. Every color forks on theme; dark is home base,
// light must hold up equally. Alphas are deliberate: washes, not paint.

Brush PopupBackgroundBrush() {
    // Solid (not theme-resource) so the borderless window reads as one crisp
    // surface. A few units of warmth over the settings chrome's flat gray —
    // felt more than seen.
    return DarkMode::isEnabled() ? ArgbBrush(255, 38, 35, 32) : ArgbBrush(255, 247, 244, 240);
}

Brush ChromeHairlineBrush() {
    // Warm hairline for satellite-surface edges (the preview flyout).
    return DarkMode::isEnabled() ? ArgbBrush(84, 190, 165, 135) : ArgbBrush(64, 130, 110, 85);
}

Brush SelectionFillBrush() {
    // The page's outgoing-bubble accent, at popup-selection strength.
    return DarkMode::isEnabled() ? ArgbBrush(62, 70, 150, 235) : ArgbBrush(44, 0, 110, 200);
}

Brush SelectionRingBrush() {
    // Crisp ring around the selected row; the fill alone went mushy on images.
    return DarkMode::isEnabled() ? ArgbBrush(130, 95, 165, 245) : ArgbBrush(110, 0, 110, 200);
}

Brush RegisterNameBrush() {
    // Registers are the app's "saved" world — the page's amber, as a name tint.
    return DarkMode::isEnabled() ? ArgbBrush(255, 235, 185, 110) : ArgbBrush(255, 146, 98, 22);
}

Brush PrivateMetaBrush() {
    // The "· private" meta line on private registers, matching the page's badge.
    return DarkMode::isEnabled() ? ArgbBrush(255, 214, 158, 88) : ArgbBrush(255, 158, 104, 26);
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

// Send a clean Ctrl+V to whoever holds the keyboard. The physical modifier
// state is scrubbed first: the user may still hold the summon chord, and a
// held Win/Alt/Shift would corrupt the paste into Win+V / Alt+V /
// paste-special. UIPI caveat (accepted for now): injection into an elevated
// window silently does nothing without a uiAccess-signed binary.
void InjectPasteChord() {
    std::vector<INPUT> inputs;
    const auto addKey = [&inputs](WORD vk, bool keyUp) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
        inputs.push_back(input);
    };
    const WORD modifiers[] = {
        VK_LWIN, VK_RWIN, VK_MENU, VK_LMENU, VK_RMENU,
        VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
    };
    for (const WORD vk : modifiers) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            addKey(vk, true);
        }
    }
    addKey(VK_CONTROL, false);
    addKey('V', false);
    addKey('V', true);
    addKey(VK_CONTROL, true);
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

// ---- Win+V-style input routing ----
// The popup is WS_EX_NOACTIVATE and NEVER takes the system foreground: the
// user's window keeps activation, Win32 focus, and its caret for the popup's
// whole life, so the paste chord lands in a window that never lost focus (the
// old restore-then-poll dance is gone, and with it its failure modes). The
// price: keyboard input routes to the foreground thread, which is never us.
// A WH_KEYBOARD_LL hook — armed only while the popup is visible — eats each
// key and re-posts it as WM_KEYDOWN/WM_KEYUP to this thread's focus window
// (the island's InputSite child), where the ordinary
// PreTranslateMessage/TranslateMessage flow turns it into XAML key events and
// WM_CHARs exactly like real input. TranslateMessage consults the THREAD's
// keyboard state, which the system stops updating for a non-foreground
// thread, so the hook snapshots its own mirrored modifier state per key and
// PreTranslateMessage applies the matching snapshot just before translating
// (applying at post time is wrong: a burst of posts would all translate
// against the LAST state). Spike-verified 2026-08-07: chars + Shift casing +
// real hardware keys work; XAML's own editing keys do NOT (see
// HandleTextEditKey). Same trick Windows can't show us: Win+V itself rides an
// OS-private input-host channel with focus nowhere at all.
//
// Everything here runs on the tray thread: LL hook callbacks fire on the
// thread that installed them (it pumps), so no locking anywhere.

constexpr UINT kHookDismissMessage = WM_APP + 0x50;

HHOOK g_popupKeyboardHook = nullptr;
HHOOK g_popupMouseHook = nullptr;
HWINEVENTHOOK g_popupForegroundHook = nullptr;
HWND g_hookPopupHwnd = nullptr;   // dismiss requests are posted here
HWND g_hookIslandHwnd = nullptr;  // key target when the thread has no focus window
BYTE g_hookKeyState[256] = {};
std::deque<std::array<BYTE, 256>> g_pendingKeyState;
struct HookChord { UINT mods; UINT vk; };
HookChord g_summonChords[2] = {};

bool IsModifierVk(UINT vk) {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        return true;
    default:
        return false;
    }
}

void TrackHookModifier(UINT vk, bool down) {
    const BYTE state = down ? 0x80 : 0x00;
    if (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) {
        // Toggle keys: keep the toggle bit truthful for TranslateMessage.
        if (down) { g_hookKeyState[vk] ^= 0x01; }
        g_hookKeyState[vk] = (g_hookKeyState[vk] & 0x01) | state;
        return;
    }
    g_hookKeyState[vk] = state;
    const auto mirror = [&](UINT left, UINT right, UINT generic) {
        if (vk == left || vk == right) {
            g_hookKeyState[generic] =
                ((g_hookKeyState[left] | g_hookKeyState[right]) & 0x80) ? 0x80 : 0x00;
        }
    };
    mirror(VK_LSHIFT, VK_RSHIFT, VK_SHIFT);
    mirror(VK_LCONTROL, VK_RCONTROL, VK_CONTROL);
    mirror(VK_LMENU, VK_RMENU, VK_MENU);
}

bool HookModifierHeld(UINT vk) { return (g_hookKeyState[vk] & 0x80) != 0; }

// The summon hotkeys must keep reaching RegisterHotKey (re-summon toggles the
// popup closed), so those exact chords pass through untouched.
bool MatchesSummonChord(UINT vk) {
    UINT mods = 0;
    if (HookModifierHeld(VK_LWIN) || HookModifierHeld(VK_RWIN)) mods |= MOD_WIN;
    if (HookModifierHeld(VK_CONTROL)) mods |= MOD_CONTROL;
    if (HookModifierHeld(VK_MENU)) mods |= MOD_ALT;
    if (HookModifierHeld(VK_SHIFT)) mods |= MOD_SHIFT;
    for (const HookChord& chord : g_summonChords) {
        if (chord.vk != 0 && chord.vk == vk && chord.mods == mods) {
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK PopupKeyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION || g_hookPopupHwnd == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    const UINT vk = key->vkCode;

    // Modifiers pass through (the foreground app's picture of Shift/Ctrl/Alt
    // stays coherent); the snapshots below carry their state to our chars.
    if (IsModifierVk(vk)) {
        TrackHookModifier(vk, !up);
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    // The OS keeps its chords: Win combos (Win+L, Win+Shift+S, ...), the
    // configured summon hotkeys (RegisterHotKey must fire for toggle-close),
    // the Alt window switcher, and hardware media keys.
    if (HookModifierHeld(VK_LWIN) || HookModifierHeld(VK_RWIN)
        || MatchesSummonChord(vk)
        || (HookModifierHeld(VK_MENU) && (vk == VK_TAB || vk == VK_ESCAPE))
        || (vk >= VK_VOLUME_MUTE && vk <= VK_MEDIA_PLAY_PAUSE)) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Ours: snapshot the modifier state this key was struck under, post it to
    // the island, and swallow the original. Callback stays trivially fast
    // (LL hooks that dawdle get silently uninstalled).
    std::array<BYTE, 256> snapshot;
    memcpy(snapshot.data(), g_hookKeyState, sizeof(g_hookKeyState));
    snapshot[vk] = up ? 0x00 : 0x80;
    g_pendingKeyState.push_back(snapshot);

    UINT scan = key->scanCode & 0xFFu;
    if (scan == 0) { scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); }
    const LPARAM keyLParam = 1 | (static_cast<LPARAM>(scan) << 16)
        | ((key->flags & LLKHF_EXTENDED) ? (1LL << 24) : 0)
        | (up ? ((1LL << 30) | (1LL << 31)) : 0);
    const HWND focus = GetFocus();
    PostMessageW(focus != nullptr ? focus : g_hookIslandHwnd,
        up ? WM_KEYUP : WM_KEYDOWN, vk, keyLParam);
    return 1;
}

// Click-outside light dismiss. The popup never activates, so WM_ACTIVATE
// can't signal focus loss anymore; instead any button-down over a window of
// another thread (or over nothing) closes the popup. Never eats the click —
// it lands where the user aimed it.
LRESULT CALLBACK PopupMouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_hookPopupHwnd != nullptr) {
        switch (wParam) {
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: {
            const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            const HWND hit = WindowFromPoint(mouse->pt);
            if (hit == nullptr
                || GetWindowThreadProcessId(hit, nullptr) != GetCurrentThreadId()) {
                PostMessageW(g_hookPopupHwnd, kHookDismissMessage, 0, 0);
            }
            break;
        }
        default:
            break;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Alt+Tab and friends: a foreground change to any foreign window dismisses.
// WINEVENT_OUTOFCONTEXT delivers on our own pump — no locking, no injection.
void CALLBACK PopupForegroundEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG,
                                       DWORD, DWORD) {
    if (g_hookPopupHwnd != nullptr && hwnd != nullptr
        && GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) {
        PostMessageW(g_hookPopupHwnd, kHookDismissMessage, 0, 0);
    }
}

void ArmPopupInputHooks(HWND popup, HWND island) {
    g_hookPopupHwnd = popup;
    g_hookIslandHwnd = island;
    g_pendingKeyState.clear();
    // Seed modifier tracking from live hardware state: the summon chord is
    // usually still held at this instant.
    memset(g_hookKeyState, 0, sizeof(g_hookKeyState));
    const UINT seeds[] = { VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                           VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
    for (const UINT vk : seeds) {
        if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) {
            TrackHookModifier(vk, true);
        }
    }
    if (GetKeyState(VK_CAPITAL) & 0x01) { g_hookKeyState[VK_CAPITAL] = 0x01; }
    g_summonChords[0] = { g_settings.popupHotkeyPrimary() >> 16,
                          g_settings.popupHotkeyPrimary() & 0xFFFFu };
    g_summonChords[1] = { g_settings.popupHotkeySecondary() >> 16,
                          g_settings.popupHotkeySecondary() & 0xFFFFu };
    if (g_popupKeyboardHook == nullptr) {
        g_popupKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, PopupKeyboardHookProc, nullptr, 0);
    }
    if (g_popupMouseHook == nullptr) {
        g_popupMouseHook = SetWindowsHookExW(WH_MOUSE_LL, PopupMouseHookProc, nullptr, 0);
    }
    if (g_popupForegroundHook == nullptr) {
        g_popupForegroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, PopupForegroundEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    }
    if (g_popupKeyboardHook == nullptr) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Popup keyboard hook failed to install (%lu): popup will be mouse-only this session.",
            GetLastError());
    }
}

void DisarmPopupInputHooks() {
    if (g_popupKeyboardHook != nullptr) {
        UnhookWindowsHookEx(g_popupKeyboardHook);
        g_popupKeyboardHook = nullptr;
    }
    if (g_popupMouseHook != nullptr) {
        UnhookWindowsHookEx(g_popupMouseHook);
        g_popupMouseHook = nullptr;
    }
    if (g_popupForegroundHook != nullptr) {
        UnhookWinEvent(g_popupForegroundHook);
        g_popupForegroundHook = nullptr;
    }
    g_pendingKeyState.clear();
    g_hookPopupHwnd = nullptr;
    g_hookIslandHwnd = nullptr;
}

// Editing keys, done by hand. Characters reach the TextBox through the hook
// pipeline as genuine WM_CHARs, but XAML's own text EDITING (Backspace,
// Delete, caret motion) sits behind input machinery that posted key messages
// never reach (spike-verified: KeyDown fires, nothing edits). So the popup
// implements the small editing vocabulary directly on the box: Backspace and
// Delete (plain + Ctrl word variants), caret motion with full Shift
// extension, and Ctrl+A. Caret ops are surrogate-pair aware. Returns true
// when the key was one of ours.

// The caret sits at one EDGE of the selection, but XAML only exposes
// (start, length) — Shift-extension needs to know which edge moves. Every
// keyboard selection change flows through HandleTextEditKey, so track the
// edge here; only one box is ever being edited at a time. A mouse-made
// selection defaults to caret-at-right (the dominant left-to-right drag), so
// the first Shift+motion after one can anchor on the wrong edge — accepted.
bool g_textEditCaretAtRight = true;

bool HandleTextEditKey(TextBox const& box, winrt::Windows::System::VirtualKey key) {
    using winrt::Windows::System::VirtualKey;
    if (!box) {
        return false;
    }
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    std::wstring text{ box.Text() };
    const int32_t size = static_cast<int32_t>(text.size());
    int32_t selStart = box.SelectionStart();
    int32_t selLen = box.SelectionLength();
    selStart = std::clamp(selStart, 0, size);
    selLen = std::clamp(selLen, 0, size - selStart);

    const auto prevBoundary = [&text](int32_t pos) {
        if (pos <= 0) return 0;
        int32_t p = pos - 1;
        if (p > 0 && IS_LOW_SURROGATE(text[p]) && IS_HIGH_SURROGATE(text[p - 1])) --p;
        return p;
    };
    const auto nextBoundary = [&text, size](int32_t pos) {
        if (pos >= size) return size;
        int32_t p = pos + 1;
        if (p < size && IS_HIGH_SURROGATE(text[pos]) && IS_LOW_SURROGATE(text[p])) ++p;
        return p;
    };
    const auto prevWord = [&text, &prevBoundary](int32_t pos) {
        int32_t p = pos;
        while (p > 0 && iswspace(text[p - 1])) p = prevBoundary(p);
        while (p > 0 && !iswspace(text[p - 1])) p = prevBoundary(p);
        return p;
    };
    const auto nextWord = [&text, size, &nextBoundary](int32_t pos) {
        int32_t p = pos;
        while (p < size && !iswspace(text[p])) p = nextBoundary(p);
        while (p < size && iswspace(text[p])) p = nextBoundary(p);
        return p;
    };
    const auto commit = [&box, &text](int32_t caret) {
        box.Text(winrt::hstring{ text });  // fires TextChanged (filter re-runs)
        box.SelectionStart(caret);
        box.SelectionLength(0);
    };

    switch (key) {
    case VirtualKey::Back:
        if (selLen > 0) {
            text.erase(static_cast<size_t>(selStart), static_cast<size_t>(selLen));
            commit(selStart);
        } else {
            const int32_t from = ctrl ? prevWord(selStart) : prevBoundary(selStart);
            if (from >= selStart) {
                return true;  // nothing to delete; consume without a re-render
            }
            text.erase(static_cast<size_t>(from), static_cast<size_t>(selStart - from));
            commit(from);
        }
        return true;
    case VirtualKey::Delete:
        if (selLen > 0) {
            text.erase(static_cast<size_t>(selStart), static_cast<size_t>(selLen));
        } else {
            const int32_t to = ctrl ? nextWord(selStart) : nextBoundary(selStart);
            if (to <= selStart) {
                return true;  // nothing to delete; consume without a re-render
            }
            text.erase(static_cast<size_t>(selStart), static_cast<size_t>(to - selStart));
        }
        commit(selStart);
        return true;
    case VirtualKey::Left:
    case VirtualKey::Right:
    case VirtualKey::Home:
    case VirtualKey::End: {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const int32_t selEnd = selStart + selLen;
        const int32_t caret = (selLen == 0 || g_textEditCaretAtRight) ? selEnd : selStart;
        const int32_t anchor = (selLen == 0) ? caret
            : (g_textEditCaretAtRight ? selStart : selEnd);
        int32_t target;
        if (key == VirtualKey::Left) {
            target = ctrl ? prevWord(caret) : prevBoundary(caret);
        } else if (key == VirtualKey::Right) {
            target = ctrl ? nextWord(caret) : nextBoundary(caret);
        } else if (key == VirtualKey::Home) {
            target = 0;
        } else {
            target = size;
        }
        if (shift) {
            // Extend: the anchor edge stays put, the caret edge moves (and
            // may cross the anchor, flipping the selection's direction).
            box.SelectionStart(std::min(anchor, target));
            box.SelectionLength(std::max(anchor, target) - std::min(anchor, target));
            g_textEditCaretAtRight = target >= anchor;
        } else if (selLen > 0 && (key == VirtualKey::Left || key == VirtualKey::Right)) {
            // Plain arrow with a selection: collapse to that edge (native
            // semantics), no motion.
            box.SelectionStart(key == VirtualKey::Left ? selStart : selEnd);
            box.SelectionLength(0);
        } else {
            box.SelectionStart(target);
            box.SelectionLength(0);
        }
        return true;
    }
    case VirtualKey::A:
        if (ctrl) {
            box.SelectAll();
            g_textEditCaretAtRight = true;
            return true;
        }
        return false;
    default:
        return false;
    }
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

    // For the popup's own-window check (never mistake a satellite for the
    // user's paste target).
    HWND Hwnd() const { return hwnd_; }

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

    // See ToastWindow::Hwnd.
    HWND Hwnd() const { return hwnd_; }

    void Destroy() {
        // Island teardown is not allowed to throw: see the note on
        // PopupWindow::Destroy — an hresult_error escaping here rides out
        // through a COM boundary and fail-fasts the process.
        try {
            if (xamlSource_) {
                xamlSource_.Close();
                xamlSource_ = nullptr;
            }
        } catch (const winrt::hresult_error&) {
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
        root_.BorderBrush(ChromeHairlineBrush());
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
            Dismiss();
        } else {
            Summon();
        }
    }

    bool PreTranslateMessage(MSG* msg) {
        if (hwnd_ == nullptr || !IsWindowVisible(hwnd_) || !xamlSource_) {
            return false;
        }
        // Hook-reposted keys: apply the modifier snapshot that was live when
        // this key was struck, so the TranslateMessage below (in the tray
        // pump) produces the right character. The system does not maintain a
        // non-foreground thread's keyboard state — we do.
        if ((msg->message == WM_KEYDOWN || msg->message == WM_KEYUP
                || msg->message == WM_SYSKEYDOWN || msg->message == WM_SYSKEYUP)
            && !g_pendingKeyState.empty()) {
            SetKeyboardState(g_pendingKeyState.front().data());
            g_pendingKeyState.pop_front();
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

    // Teardown must be exception-PROOF, not merely exception-safe. This runs from
    // the tray window's WM_DESTROY — i.e. inside a window procedure dispatched by
    // the message loop — and a C++/WinRT hresult_error that escapes into the COM
    // boundary above us gets converted by RoFailFastWithErrorContext into a
    // STOWED EXCEPTION (0xC000027B), which no filter and no crash handler can
    // intercept. That is the exit code winget's install verification reported for
    // this build (and the same class ../WM_NIGHT hardened its own shutdown
    // against). Nothing here is worth dying for: every handle is about to be
    // abandoned by process exit anyway.
    void Destroy() {
        DisarmPopupInputHooks();  // plain user32; cannot throw
        try {
            EndActivityNotifications();
        } catch (const winrt::hresult_error&) {
        }
        toastWindow_.Destroy();     // GDI only; cannot throw WinRT
        previewWindow_.Destroy();   // guards its own island
        try {
            if (xamlSource_) {
                xamlSource_.Close();
                xamlSource_ = nullptr;
            }
        } catch (const winrt::hresult_error&) {
            xamlSource_ = nullptr;
        }
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        dispatcher_ = nullptr;
    }

private:
    // ---- lifecycle ----

    void Summon() {
        // FIRST, before anything of ours can exist or take focus: whoever the
        // user was typing in is the paste target. Creating the window (and, on
        // a cold summon, the XAML island) perturbs focus, so capturing after
        // Create() recorded the WRONG target on the first summon of a session —
        // the paste then "restored" to that and the poll never matched. Classic
        // symptom, user-reported: paste fails on the first try, works forever
        // after (every later summon skips Create()).
        const HWND foregroundAtSummon = GetForegroundWindow();

        // Every brush was baked for the OS theme current at build time (the
        // window is lazy-created-then-kept). If the theme flipped since, tear
        // the whole thing down and let the lazy-create below rebuild it fresh
        // — satellites included, same cost as a first summon, zero re-theming
        // invariants to maintain. (A flip while the popup is VISIBLE keeps the
        // old clothes until the next summon — it is a summon-transient surface.)
        if (hwnd_ != nullptr && builtForDark_ != DarkMode::isEnabled()) {
            Destroy();
        }
        if (hwnd_ == nullptr) {
            Create();
            if (hwnd_ == nullptr) {
                return;
            }
        }

        CancelPasteInjection();  // a re-summon supersedes any pending paste
        // Never target ourselves: a stale/own-window capture would make the
        // restore a no-op and send the chord into the void.
        previousForeground_ = IsOwnWindow(foregroundAtSummon) ? nullptr : foregroundAtSummon;
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            L"Popup summon: paste target %ls%ls",
            DescribeWindow(previousForeground_).c_str(),
            IsOwnWindow(foregroundAtSummon) ? L" (ignored: own window)" : L"");

        RebuildFromStores();
        UpdateColumnLayout();
        if (filterBox_) {
            filterBox_.Text(L"");  // fresh session; fires TextChanged -> SetFilter("")
        }
        RenderList();

        PositionOnCursorMonitor();
        // Shown WITHOUT activation: the paste target never loses the
        // foreground. FocusFilterBox still runs — it moves this THREAD's
        // focus into the island so the hook-reposted keys have a home.
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        FocusFilterBox();
        ArmPopupInputHooks(hwnd_, xamlHost_);

        // Summoning by any route proves the ClippPage teach banner's lesson
        // landed — retire it for good (idempotent; the page re-checks on show).
        g_settings.notePopupTeachBannerDismissed();

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

    void Dismiss() {
        if (hwnd_ == nullptr || !IsWindowVisible(hwnd_)) {
            return;
        }
        DisarmPopupInputHooks();
        if (editingRegister_.has_value()) {
            EndEditMode();  // silent cancel; the next summon rebuilds the rows
        }
        EndActivityNotifications();
        toastWindow_.Hide();
        previewWindow_.Hide();
        // A pending Type confirmation does not survive the popup closing: the
        // next summon must start from a clean, unarmed state.
        DisarmType();
        HideTypeBubble();
        // Session-scoped peeks: anything revealed inside the popup is
        // forgotten the moment it hides.
        uiClippPage::ForgetAllPeekedItems();
        peekedRegisterNames_.clear();
        ShowWindow(hwnd_, SW_HIDE);
        // The target held the foreground throughout — no restoration to do.
        // The one exception: something of OURS grabbed it anyway (XAML
        // context-menu flyouts can activate their own popup HWNDs); only then
        // hand the foreground back to the recorded target.
        const HWND foreground = GetForegroundWindow();
        const bool oursHoldsForeground = foreground != nullptr
            && (IsOwnWindow(foreground)
                || GetWindowThreadProcessId(foreground, nullptr) == GetCurrentThreadId());
        if (oursHoldsForeground && previousForeground_ != nullptr && IsWindow(previousForeground_)) {
            SetForegroundWindow(previousForeground_);
        }
        previousForeground_ = nullptr;
    }

    // True for the popup itself, its island host, and its satellites — anything
    // whose focus must never be mistaken for the user's paste target.
    bool IsOwnWindow(HWND hwnd) const {
        if (hwnd == nullptr) {
            return false;
        }
        const HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
        for (const HWND own : { hwnd_, xamlHost_, previewWindow_.Hwnd(), toastWindow_.Hwnd() }) {
            if (own != nullptr && (hwnd == own || root == own)) {
                return true;
            }
        }
        return false;
    }

    void Create() {
        builtForDark_ = DarkMode::isEnabled();  // Summon compares on re-entry
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            L"Popup building for %ls theme (DarkMode::isEnabled=%d, registry AppsUseLightTheme=%d).",
            builtForDark_ ? L"DARK" : L"LIGHT", builtForDark_ ? 1 : 0,
            AppsUseLightThemeRegValue());
        RegisterPopupClass();
        const HINSTANCE hInstance = GetModuleHandleW(nullptr);
        // NOACTIVATE is the Win+V trick: the popup never takes the system
        // foreground, so the paste target keeps focus/caret for the popup's
        // whole life and the paste chord needs no focus restoration at all.
        // Keyboard reaches the island via the LL-hook repost pipeline above.
        CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
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
            Dismiss();
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
        pasteButton_ = MakeToolbarButton(L"\xE77F", CLP_W(CLP_UI_PASTE));
        pasteButton_.Click([this](auto const&, auto const&) {
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
        undoButton_ = MakeToolbarButton(L"\xE7A7", CLP_W(CLP_UI_POPUP_UNDO_TIP));
        undoButton_.Click([this](auto const&, auto const&) {
            UndoLastDelete();
        });
        // Keyboard glyph: type the item out instead of pasting it.
        typeButton_ = MakeToolbarButton(L"\xE765", CLP_W(CLP_UI_POPUP_TYPE_TIP));
        typeButton_.Click([this](auto const&, auto const&) {
            TypeSelected();
        });
        toolbar.Children().Append(saveButton_);
        toolbar.Children().Append(pasteButton_);
        toolbar.Children().Append(typeButton_);
        toolbar.Children().Append(renameButton_);
        toolbar.Children().Append(privateButton_);
        toolbar.Children().Append(deleteButton_);
        toolbar.Children().Append(undoButton_);
        Grid::SetRow(toolbar, 1);
        root.Children().Append(toolbar);

        // Confirm / error bubble, anchored under the Type button. Lives as a
        // root-Grid overlay (not a XAML Flyout) so it can never take focus from
        // the filter box or trip the popup's light-dismiss.
        typeBubble_ = BuildTypeBubble();
        Grid::SetRow(typeBubble_, 1);
        Grid::SetRowSpan(typeBubble_, 3);
        root.Children().Append(typeBubble_);
        contentRoot_ = root;  // the bubble anchors against this coordinate space

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
            // Small-caps treatment: uppercase + letter tracking reads as a
            // designed caption instead of a shrunken heading.
            std::wstring upper{ text };
            for (auto& ch : upper) {
                ch = static_cast<wchar_t>(towupper(ch));
            }
            TextBlock label;
            label.Text(winrt::hstring{ upper });
            label.FontSize(11);
            label.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            label.CharacterSpacing(70);  // 1/1000 em units
            label.Opacity(0.55);
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
            // caret to move through; with text, the caret motion is ours to
            // implement (hook-pipeline keys don't reach XAML's text editing).
            if (filterEmpty) {
                if (key == VirtualKey::Left) model_.MoveLeft(); else model_.MoveRight();
                RenderHighlight();
                args.Handled(true);
            } else if (HandleTextEditKey(filterBox_, key)) {
                args.Handled(true);
            }
            return;
        case VirtualKey::Enter:
            ActivateSelected();
            args.Handled(true);
            return;
        case VirtualKey::Back:
        case VirtualKey::Home:
        case VirtualKey::End:
            if (HandleTextEditKey(filterBox_, key)) {
                args.Handled(true);
            }
            return;
        case VirtualKey::A:
            // Ctrl+A selects the filter text; a plain 'a' keeps flowing in.
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && HandleTextEditKey(filterBox_, key)) {
                args.Handled(true);
            }
            return;
        case VirtualKey::Delete:
            // With filter text present, Delete edits the text; on an empty
            // filter it deletes the selected item everywhere.
            if (filterEmpty) {
                DeleteSelected();
                args.Handled(true);
            } else if (HandleTextEditKey(filterBox_, key)) {
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
        case VirtualKey::T:
            // Ctrl+T types the selection out instead of pasting it; a plain
            // 't' keeps flowing into the filter.
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                TypeSelected();
                args.Handled(true);
            }
            return;
        case VirtualKey::Z:
            // Ctrl+Z restores the last delete while one is armed; otherwise
            // the TextBox keeps its own text-undo of the filter.
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0
                && clipp::PendingUndoKind() != clipp::UndoSlotKind::None) {
                UndoLastDelete();
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
                Dismiss();
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
        bool isBinary = false;     // image/stream register: nothing to type out
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
            info.isBinary = rec.IsBinary();
            info.touchedWallMs = rec.touched.wallMs;

            PopupItem item;
            item.kind = PopupItem::Kind::Register;
            item.registerName = rec.name;
            // Unlike history kind-labels, register NAMES are user data — the
            // primary handle — so they participate in find (and light up).
            item.searchText = info.name;

            if (rec.IsPrivate() && peekedRegisterNames_.count(rec.name) == 0) {
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
                // Preview from the leading-whitespace-trimmed text: a value that
                // opens with newlines would otherwise show its blank first line
                // and render as an empty row. fullText (the flyout) stays exact.
                const std::wstring shown = TrimLeadingWhitespace(info.fullText);
                info.previewText = shown.size() > kRegisterPreviewChars
                    ? shown.substr(0, kRegisterPreviewChars) + L"..."
                    : shown;
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

    // The eye on a masked row: a small trailing button toggling the (session-
    // scoped) peek. `peeked` picks the glyph; `onToggle` flips the state and
    // re-renders. Returns the row child: the content wrapped in a grid with
    // the eye in a trailing auto column.
    Grid WrapWithPeekButton(StackPanel const& content, bool peeked,
                            std::function<void()> onToggle) {
        FontIcon eyeIcon;
        eyeIcon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
        eyeIcon.FontSize(13);
        eyeIcon.Glyph(peeked ? L"\xED1A" : L"\xE7B3");

        Button eye;
        eye.Content(eyeIcon);
        eye.MinWidth(0);
        eye.MinHeight(0);
        eye.Width(26);
        eye.Height(26);
        eye.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
        eye.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 0));
        eye.Background(ArgbBrush(0, 0, 0, 0));
        eye.Opacity(0.65);
        eye.VerticalAlignment(VerticalAlignment::Center);
        ToolTipService::SetToolTip(eye, winrt::box_value(winrt::hstring{
            peeked ? CLP_W(CLP_UI_PEEK_HIDE) : CLP_W(CLP_UI_PEEK) }));
        eye.Click([onToggle = std::move(onToggle)](auto const&, auto const&) {
            onToggle();
        });

        Grid wrap;
        ColumnDefinition contentColumn;
        contentColumn.Width(GridLength{ 1, GridUnitType::Star });
        ColumnDefinition eyeColumn;
        eyeColumn.Width(GridLength{ 1, GridUnitType::Auto });
        wrap.ColumnDefinitions().Append(contentColumn);
        wrap.ColumnDefinitions().Append(eyeColumn);
        Grid::SetColumn(content, 0);
        wrap.Children().Append(content);
        Grid::SetColumn(eye, 1);
        wrap.Children().Append(eye);
        return wrap;
    }

    Border BuildRow(const PopupItem& item, std::size_t index) {
        const auto cached = displayCache_.find(item.historyId);
        // Masked-private text carries its unmasked twin; that (and only that)
        // is what the eye can reveal. Placeholders have no content to show.
        const bool peekable = cached != displayCache_.end()
            && !cached->second.revealedPreviewText.empty();
        const bool peeked = peekable && uiClippPage::IsItemPeeked(item.historyId);

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
                previewText = peeked ? display.revealedPreviewText
                                     : display.previewText;
                break;
            }
            contentRow = display.kind == ClipboardActivityPayloadKind::Text ||
                         display.kind == ClipboardActivityPayloadKind::Link;
            // "Mars11 14 seconds ago" — who it came from, and how fresh.
            // eventTimestamp (origin time), not header.timestamp (learn time):
            // a restart re-seed would otherwise label every row "now".
            metaText = display.deviceName.empty()
                ? RelativeAgeText(display.eventTimestamp)
                : display.deviceName + L" " + RelativeAgeText(display.eventTimestamp);
        }
        if (previewText.empty()) {
            previewText = L" ";
        }
        // Find applies to content only — labels and meta lines are exempt from
        // both matching and highlighting. A content match past the single-line
        // ellipsis would be invisible, so re-window the text around the first
        // match instead of scrolling anything.
        if (contentRow) {
            previewText = popupfind::ReWindowRowText(std::move(previewText), model_.Filter());
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
        // 1px border lives on EVERY row (transparent until selected) with the
        // padding compensated, so selection never shifts content by a pixel.
        row.Padding(ThicknessHelper::FromLengths(9, 5, 9, 5));
        row.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
        row.BorderBrush(ArgbBrush(0, 0, 0, 0));
        row.CornerRadius(CornerRadius{ 6 });
        row.Background(ArgbBrush(0, 0, 0, 0));
        const auto toggleHistoryPeek = [this, id = item.historyId, index]() {
            uiClippPage::ToggleItemPeeked(id);
            model_.SelectAt(PopupModel::Group::History, index);
            RenderList();
        };
        if (peekable) {
            row.Child(WrapWithPeekButton(content, peeked, toggleHistoryPeek));
        } else {
            row.Child(content);
        }

        row.PointerPressed([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            RenderHighlight();
        });
        row.DoubleTapped([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            ActivateSelected();
        });

        MenuFlyout menu;
        MenuFlyoutItem pasteItem;
        pasteItem.Text(winrt::hstring{ CLP_W(CLP_UI_PASTE) });
        pasteItem.Click([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            ActivateSelected();
        });
        menu.Items().Append(pasteItem);
        if (peekable) {
            MenuFlyoutItem peekItem;
            peekItem.Text(winrt::hstring{
                peeked ? CLP_W(CLP_UI_PEEK_HIDE) : CLP_W(CLP_UI_PEEK) });
            peekItem.Click([toggleHistoryPeek](auto const&, auto const&) {
                toggleHistoryPeek();
            });
            menu.Items().Append(peekItem);
        }
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
                    // Backspace/Delete/caret keys are ours to implement under
                    // the hook pipeline (see HandleTextEditKey).
                    if (HandleTextEditKey(nameEditor_, args.Key())) {
                        args.Handled(true);
                    }
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
            // The saved-things column wears the app's amber; the find
            // highlighter (forced black on amber) still wins on matches.
            name.Foreground(RegisterNameBrush());
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
        if (contentRow) {
            previewText = popupfind::ReWindowRowText(std::move(previewText), model_.Filter());
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
                // Private rows' meta line takes the page's badge amber.
                meta.Foreground(PrivateMetaBrush());
            }
            meta.Text(winrt::hstring{ metaText });
            meta.FontSize(11);
            meta.Opacity(0.6);
            content.Children().Append(meta);
        }

        Border row;
        // Same constant-border discipline as the history rows.
        row.Padding(ThicknessHelper::FromLengths(9, 5, 9, 5));
        row.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
        row.BorderBrush(ArgbBrush(0, 0, 0, 0));
        row.CornerRadius(CornerRadius{ 6 });
        row.Background(ArgbBrush(0, 0, 0, 0));
        const bool privateRegister =
            cached != registerCache_.end() && cached->second.isPrivate;
        const bool peeked =
            privateRegister && peekedRegisterNames_.count(item.registerName) != 0;
        if (privateRegister && !editing) {
            row.Child(WrapWithPeekButton(content, peeked,
                [this, name = item.registerName]() { TogglePeekedRegister(name); }));
        } else {
            row.Child(content);
        }

        row.PointerPressed([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::Registers, index);
            RenderHighlight();
        });
        row.DoubleTapped([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::Registers, index);
            ActivateSelected();
        });

        MenuFlyout menu;
        MenuFlyoutItem pasteItem;
        pasteItem.Text(winrt::hstring{ CLP_W(CLP_UI_PASTE) });
        pasteItem.Click([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::Registers, index);
            ActivateSelected();
        });
        menu.Items().Append(pasteItem);
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
        if (privateRegister) {
            MenuFlyoutItem peekItem;
            peekItem.Text(winrt::hstring{
                peeked ? CLP_W(CLP_UI_PEEK_HIDE) : CLP_W(CLP_UI_PEEK) });
            peekItem.Click([this, name = item.registerName](auto const&, auto const&) {
                TogglePeekedRegister(name);
            });
            menu.Items().Append(peekItem);
        }
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

    // Peek is render state, not a store op: the rebuild applies (or lifts) the
    // mask via the cache, the row stays selected, and the undo slot is
    // untouched. Session-scoped — Dismiss clears the set.
    void TogglePeekedRegister(const std::string& name) {
        if (peekedRegisterNames_.erase(name) == 0) {
            peekedRegisterNames_.insert(name);
        }
        RefreshAfterRegisterOp(name);
    }

    void RenderHighlight() {
        const auto selection = model_.Selected();
        const auto paint = [this, &selection](std::vector<Border>& borders, PopupModel::Group group) {
            for (std::size_t i = 0; i < borders.size(); ++i) {
                const bool selected = selection.has_value()
                    && selection->group == group
                    && selection->index == i;
                // Accent wash + crisp ring (was a gray-on-gray wash — the
                // "dour" read). The border is always 1px, so no pixel shift.
                borders[i].Background(selected ? SelectionFillBrush()
                                               : ArgbBrush(0, 0, 0, 0));
                borders[i].BorderBrush(selected ? SelectionRingBrush()
                                                : ArgbBrush(0, 0, 0, 0));
                if (selected) {
                    RevealRow(group, borders[i]);
                }
            }
        };
        paint(registerRowBorders_, PopupModel::Group::Registers);
        paint(rowBorders_, PopupModel::Group::History);
        UpdateToolbar();
        SchedulePreviewFlyoutUpdate();
    }

    // Scroll `row` into view inside its column. Replaces StartBringIntoView,
    // which silently no-ops on a freshly rebuilt row (no realized geometry) —
    // deferring a dispatcher hop was NOT enough, because a hop is not reliably
    // after XAML's arrange pass. User repro that survived the hop fix: save an
    // image register, name it "zzzitem" so it sorts last, and the new row stays
    // off-view. So: force the arrange, then scroll deterministically by offset
    // — and only when the row isn't already fully visible (the mac popup's
    // proven "don't touch a row that's already contained" discipline).
    void RevealRow(PopupModel::Group group, Border const& row) {
        ScrollViewer scroll = group == PopupModel::Group::Registers ? registerScroll_ : listScroll_;
        if (!scroll || !row) {
            return;
        }
        scroll.UpdateLayout();  // the FLIP/RowAnchorScreenY lesson: never trust fresh geometry
        try {
            const auto content = scroll.Content().try_as<UIElement>();
            if (!content) {
                row.StartBringIntoView();
                return;
            }
            const auto top = row.TransformToVisual(content)
                .TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 }).Y;
            const double rowHeight = row.ActualHeight();
            const double viewTop = scroll.VerticalOffset();
            const double viewHeight = scroll.ViewportHeight();
            if (viewHeight <= 0.0) {
                return;  // not laid out yet; the next render reveals it
            }
            if (top < viewTop) {
                scroll.ChangeView(nullptr, static_cast<double>(top), nullptr, true);
            } else if (top + rowHeight > viewTop + viewHeight) {
                scroll.ChangeView(nullptr, top + rowHeight - viewHeight, nullptr, true);
            }
        } catch (const winrt::hresult_error&) {
            row.StartBringIntoView();  // last resort; better than nothing
        }
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
        if (pasteButton_) {
            pasteButton_.IsEnabled(item != nullptr && item->actionable);
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
        if (typeButton_) {
            // Text only: an image has no keystrokes. Registers carry their own
            // binary flag; history rows are classified in the display cache.
            bool canType = false;
            if (item != nullptr && item->actionable) {
                if (item->kind == PopupItem::Kind::Register) {
                    const auto cached = registerCache_.find(item->registerName);
                    canType = cached == registerCache_.end() || !cached->second.isBinary;
                } else {
                    const auto cached = displayCache_.find(item->historyId);
                    canType = cached != displayCache_.end()
                        && (cached->second.kind == ClipboardActivityPayloadKind::Text
                            || cached->second.kind == ClipboardActivityPayloadKind::Link);
                }
            }
            typeButton_.IsEnabled(canType);
        }
        if (undoButton_) {
            const auto undoKind = clipp::PendingUndoKind();
            undoButton_.IsEnabled(undoKind != clipp::UndoSlotKind::None);
            // Name the victim when we can — "what comes back?" is the whole
            // question this button answers.
            std::wstring tip = CLP_W(CLP_UI_POPUP_UNDO_TIP);
            if (undoKind == clipp::UndoSlotKind::Register) {
                const std::wstring name = Utf8ToWideString(clipp::PendingUndoLabel());
                if (!name.empty()) {
                    tip = CLP_W(CLP_UI_POPUP_UNDO_OF_PREFIX) L"\"" + name + L"\" (Ctrl+Z)";
                }
            }
            ToolTipService::SetToolTip(undoButton_, winrt::box_value(winrt::hstring{ tip }));
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
                        const UINT dpi = GetDpiForWindow(hwnd_);
                        const int anchor = popupRect.top + DipsToPixels(point.Y, dpi);
                        // Clamp into the popup's own vertical span (the mac
                        // panel already did this): a row scrolled out of its
                        // column still transforms to a real — but off-panel —
                        // Y, and the flyout would fly off to a coordinate no
                        // row occupies. There IS no right Y for an unseen row,
                        // so pin it to the nearest edge of the surface it
                        // belongs to.
                        const int lo = static_cast<int>(popupRect.top);
                        const int hi = (std::max)(lo,
                            static_cast<int>(popupRect.bottom) - DipsToPixels(48, dpi));
                        return (std::min)((std::max)(anchor, lo), hi);
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
        const std::wstring shown = popupfind::WindowAroundFirstMatch(full, filter);
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
            // A peeked private-text item graduates to a content flyout like
            // any other text row — the peek would be half a reveal otherwise.
            if (!display.revealedPreviewText.empty()
                && uiClippPage::IsItemPeeked(item->historyId)) {
                const std::wstring& revealed = display.revealedPreviewText;
                if (TextFitsInRow(revealed)) {
                    previewWindow_.Hide();
                    return;
                }
                ShowTextFlyoutWindowed(revealed, /*preferLeft=*/false);
                return;
            }
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

    // ---- "type it out" -----------------------------------------------------
    // Anchored bubble: a pointer nub aimed at the Type button over a rounded
    // text plate. Positioned in ShowTypeBubble once layout has run.
    StackPanel BuildTypeBubble() {
        StackPanel panel;
        panel.Orientation(Orientation::Vertical);
        panel.HorizontalAlignment(HorizontalAlignment::Left);
        panel.VerticalAlignment(VerticalAlignment::Top);
        panel.Visibility(Visibility::Collapsed);
        panel.IsHitTestVisible(false);  // pure signage; clicks belong to the button

        const Brush plate = TypeBubbleBrush();
        // The nub is a square rotated 45 degrees, sunk into the plate so only
        // its top corner shows — same trick as a CSS speech-bubble tail.
        Grid nubRow;
        nubRow.Height(6);
        nubRow.HorizontalAlignment(HorizontalAlignment::Left);
        typeBubblePointer_ = Border();
        typeBubblePointer_.Width(10);
        typeBubblePointer_.Height(10);
        typeBubblePointer_.Background(plate);
        typeBubblePointer_.HorizontalAlignment(HorizontalAlignment::Left);
        typeBubblePointer_.VerticalAlignment(VerticalAlignment::Top);
        typeBubblePointer_.Margin(ThicknessHelper::FromLengths(0, 1, 0, 0));
        RotateTransform rotate;
        rotate.Angle(45);
        rotate.CenterX(5);
        rotate.CenterY(5);
        typeBubblePointer_.RenderTransform(rotate);
        nubRow.Children().Append(typeBubblePointer_);
        panel.Children().Append(nubRow);

        Border plateBorder;
        plateBorder.Background(plate);
        plateBorder.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
        plateBorder.Padding(ThicknessHelper::FromLengths(10, 6, 10, 7));
        plateBorder.MaxWidth(300);
        typeBubbleText_ = TextBlock();
        typeBubbleText_.FontSize(12);
        typeBubbleText_.TextWrapping(TextWrapping::Wrap);
        typeBubbleText_.Foreground(ArgbBrush(255, 255, 255, 255));
        plateBorder.Child(typeBubbleText_);
        panel.Children().Append(plateBorder);
        return panel;
    }

    Brush TypeBubbleBrush() {
        // Reads as an instruction, not chrome: the page's accent, opaque.
        return DarkMode::isEnabled() ? ArgbBrush(255, 38, 92, 160) : ArgbBrush(255, 20, 82, 150);
    }

    void ShowTypeBubble(const std::wstring& text) {
        if (!typeBubble_ || !typeBubbleText_ || !contentRoot_ || !typeButton_) {
            return;
        }
        typeBubbleText_.Text(winrt::hstring{ text });
        // Anchor under the Type button, in the root's coordinate space.
        double left = 12;
        double top = 0;
        try {
            const auto transform = typeButton_.TransformToVisual(contentRoot_);
            const auto origin = transform.TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
            left = origin.X;
            top = origin.Y + typeButton_.ActualHeight() + 2;
        } catch (const winrt::hresult_error&) {
            // Not laid out yet (bubble raised before the first measure pass):
            // the default margin still puts it under the toolbar.
        }
        typeBubble_.Margin(ThicknessHelper::FromLengths(left, top, 0, 0));
        if (typeBubblePointer_) {
            // Center the nub on the button, clamped to the plate.
            const double nub = (std::max)(0.0, typeButton_.ActualWidth() / 2.0 - 5.0);
            typeBubblePointer_.Margin(ThicknessHelper::FromLengths(nub, 1, 0, 0));
        }
        typeBubble_.Visibility(Visibility::Visible);
    }

    void HideTypeBubble() {
        if (typeBubble_) {
            typeBubble_.Visibility(Visibility::Collapsed);
        }
    }

    // Identity of the selected row, so an arm can't survive a selection change
    // (the confirmation quoted counts for a DIFFERENT item).
    std::wstring SelectedItemKey() const {
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr) {
            return {};
        }
        return item->kind == PopupItem::Kind::Register
            ? L"R:" + Utf8ToWideString(item->registerName)
            : L"H:" + std::to_wstring(item->historyId);
    }

    // Armed = the button is lit and the bubble is up; the next click runs.
    void SetTypeArmed(bool armed) {
        typeArmed_ = armed;
        if (typeButton_) {
            if (armed) {
                typeButton_.Background(TypeBubbleBrush());
            } else {
                // ClearValue, not Background(nullptr): the latter would pin a
                // null local value and cost the button its themed rest/hover
                // brushes for good.
                typeButton_.ClearValue(Control::BackgroundProperty());
            }
        }
        typeArmedKey_ = armed ? SelectedItemKey() : std::wstring{};
        if (armed) {
            SetTimer(hwnd_, kTypeArmTimerId, 4000, nullptr);
        } else {
            KillTimer(hwnd_, kTypeArmTimerId);
        }
    }

    void DisarmType() {
        if (typeArmed_) {
            SetTypeArmed(false);
        }
    }

    void TypeSelected() {
        DismissHintToast();
        CommitOrCancelRename();
        // Re-summoning mid-run and hitting the button again means "stop".
        if (clipp::IsTyping()) {
            clipp::CancelTyping();
            DisarmType();
            HideTypeBubble();
            return;
        }
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr || !item->actionable) {
            return;
        }
        // Selection moved since the confirmation was raised: that bubble's
        // counts described another item, so make them earn a fresh one.
        if (typeArmed_ && typeArmedKey_ != SelectedItemKey()) {
            DisarmType();
        }

        std::string utf8;
        const bool haveText = item->kind == PopupItem::Kind::History
            ? clipp::TryGetActivityItemText(item->historyId, utf8)
            : clipp::TryGetRegisterText(item->registerName, utf8);
        if (!haveText) {
            DisarmType();
            ShowTypeBubble(CLP_W(CLP_UI_POPUP_TYPE_NO_TEXT));
            return;
        }
        // An IME has no character-to-keystroke map at all; refuse rather than
        // type garbage into a composition window.
        if (clipp::ActiveLayoutIsIme()) {
            DisarmType();
            ShowTypeBubble(CLP_W(CLP_UI_POPUP_TYPE_IME));
            return;
        }

        const clipp::TypeResult result = clipp::TranslateTextToPlan(Utf8ToWideString(utf8));
        if (!result.ok) {
            DisarmType();
            ShowTypeBubble(DescribeTypeError(result.error));
            return;
        }
        clipp::TypeSchedule schedule = clipp::BuildTypeSchedule(result.plan);
        const int seconds = clipp::EstimateTypeSeconds(schedule);

        // Arm-and-confirm for anything with consequences: every Enter may run a
        // command on the far side, and a run measured in tens of seconds is
        // usually a mis-click. Short, Enter-free text types on the first click.
        if (!typeArmed_ && (schedule.enterCount > 0 || seconds >= 10)) {
            SetTypeArmed(true);
            ShowTypeBubble(DescribeTypeConfirmation(schedule, seconds));
            return;
        }

        DisarmType();
        HideTypeBubble();
        const HWND target = previousForeground_;
        Dismiss();
        if (target != nullptr && IsWindow(target)) {
            // Same activation-polling path as the paste chord: the keystrokes
            // only start once the intended window really holds the foreground.
            pendingIsType_ = true;
            pendingTypeSchedule_ = std::move(schedule);
            BeginPasteInjection(target);
        }
    }

    std::wstring DescribeTypeConfirmation(const clipp::TypeSchedule& schedule, int seconds) const {
        std::wstring text;
        if (schedule.enterCount > 0) {
            text = CLP_W(CLP_UI_POPUP_TYPE_ENTER_PREFIX) + std::to_wstring(schedule.enterCount)
                + L"×";
            if (seconds >= 10) {
                text += L", ~" + std::to_wstring(seconds) + CLP_W(CLP_UI_POPUP_TYPE_KEYSTROKES_SUFFIX);
            }
        } else {
            text = std::to_wstring(schedule.events.size())
                + CLP_W(CLP_UI_POPUP_TYPE_KEYSTROKES_MIDDLE) + std::to_wstring(seconds)
                + CLP_W(CLP_UI_POPUP_TYPE_KEYSTROKES_SUFFIX);
        }
        text += CLP_W(CLP_UI_POPUP_TYPE_CONFIRM_SUFFIX);
        return text;
    }

    // "Can't type 'ß' (U+00DF) - line 3, col 14 [United States]"
    std::wstring DescribeTypeError(const clipp::TypeError& error) const {
        std::wstring text = CLP_W(CLP_UI_POPUP_TYPE_CANT_PREFIX);
        if (error.codepoint <= 0xFFFF && iswprint(static_cast<wint_t>(error.codepoint))) {
            text += L"'";
            text += static_cast<wchar_t>(error.codepoint);
            text += L"' ";
        }
        wchar_t hex[16] = {};
        swprintf(hex, 16, L"(U+%04X)", static_cast<unsigned>(error.codepoint));
        text += hex;
        text += CLP_W(CLP_UI_POPUP_TYPE_CANT_LINE) + std::to_wstring(error.line);
        text += CLP_W(CLP_UI_POPUP_TYPE_CANT_COLUMN) + std::to_wstring(error.column);
        const std::wstring layout = clipp::ActiveKeyboardLayoutName();
        if (!layout.empty()) {
            text += L" [" + layout + L"]";
        }
        return text;
    }

    void ActivateSelected() {
        DismissHintToast();      // button-borne invocations skip the root handler
        CommitOrCancelRename();  // an action supersedes an in-flight rename
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr || !item->actionable) {
            return;
        }
        bool applied = false;
        if (item->kind == PopupItem::Kind::History) {
            applied = clipp::ReshareActivityItem(item->historyId);
        } else {
            applied = clipp::MakeRegisterCurrent(item->registerName);
        }
        // Shift held = make-current only, no keystroke (the escape hatch for
        // apps that shouldn't receive a synthetic Ctrl+V).
        const bool wantPaste = applied && (GetKeyState(VK_SHIFT) & 0x8000) == 0;
        const HWND pasteTarget = previousForeground_;
        Dismiss();
        if (wantPaste && pasteTarget != nullptr && IsWindow(pasteTarget)) {
            BeginPasteInjection(pasteTarget);
        }
    }

    // The paste keystroke fires only once the target actually holds the
    // foreground — never into a bystander window. Under the no-activate
    // popup the target held the foreground throughout, so the poll normally
    // succeeds on its first ticks; it remains as the safety net for the rare
    // paths where something of ours stole activation (context-menu flyouts)
    // and Dismiss had to hand it back. Give up gracefully either way (the
    // clipboard is set; a manual Ctrl+V works).
    void BeginPasteInjection(HWND target) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            L"Paste injection armed for %ls; foreground now %ls",
            DescribeWindow(target).c_str(),
            DescribeWindow(GetForegroundWindow()).c_str());
        pasteTargetWindow_ = target;
        // 24 x 25ms of activation grace. The original 200ms budget expired on
        // the popup's FIRST dismissal of a session — the XAML island's
        // first-build work is still churning the thread right then, focus
        // restore lags, and the paste silently skipped (user repro: terminal
        // paste fails on try one, works every time after).
        pasteRetriesLeft_ = 24;
        pasteFocusStreak_ = 0;
        SetTimer(hwnd_, kPasteTimerId, 25, nullptr);
    }

    void CancelPasteInjection() {
        if (pasteTargetWindow_ != nullptr) {
            KillTimer(hwnd_, kPasteTimerId);
            pasteTargetWindow_ = nullptr;
        }
        pendingIsType_ = false;
        pendingTypeSchedule_ = clipp::TypeSchedule{};
    }

    void OnPasteTimer() {
        const HWND target = pasteTargetWindow_;
        if (target == nullptr || !IsWindow(target)) {
            CancelPasteInjection();
            return;
        }
        const HWND foreground = GetForegroundWindow();
        const bool targetFocused = foreground != nullptr
            && (foreground == target
                || GetAncestor(foreground, GA_ROOTOWNER) == GetAncestor(target, GA_ROOTOWNER));
        if (targetFocused) {
            // One settle tick: "holds the foreground" is not "ready for input"
            // — terminals in particular re-establish their internal keyboard
            // focus a beat after activation, and a chord fired on the very
            // first foreground sighting can vanish into that gap. Inject only
            // after two consecutive foreground confirmations (+25ms).
            if (++pasteFocusStreak_ >= 2) {
                // Take the pending work before CancelPasteInjection clears it.
                const bool typeMode = pendingIsType_;
                clipp::TypeSchedule schedule = std::move(pendingTypeSchedule_);
                CancelPasteInjection();
                g_logger.log(__FUNCTION__, Logger::Level::Debug,
                    typeMode ? L"Typing -> %ls" : L"Paste chord -> %ls",
                    DescribeWindow(foreground).c_str());
                if (typeMode) {
                    clipp::StartTyping(std::move(schedule));
                } else {
                    InjectPasteChord();
                }
                return;
            }
        } else {
            pasteFocusStreak_ = 0;
        }
        if (--pasteRetriesLeft_ <= 0) {
            g_logger.log(__FUNCTION__, Logger::Level::Debug,
                L"Paste keystroke SKIPPED: target %ls never regained focus; foreground is %ls",
                DescribeWindow(target).c_str(),
                DescribeWindow(foreground).c_str());
            CancelPasteInjection();
        }
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

    // Restore the last delete (register or activity item) mesh-wide, then
    // select the resurrected item and bring it into view — the whole point
    // is showing the user their data is back.
    void UndoLastDelete() {
        DismissHintToast();
        CommitOrCancelRename();
        const auto undoKind = clipp::PendingUndoKind();
        const std::string restoredRegister = clipp::PendingUndoLabel();
        uint64_t restoredItemID = 0;
        if (!clipp::TryUndoDelete(&restoredItemID)) {
            return;
        }
        RebuildFromStores();
        UpdateColumnLayout();
        if (undoKind == clipp::UndoSlotKind::Register) {
            const auto& regs = model_.VisibleRegisters();
            for (std::size_t i = 0; i < regs.size(); ++i) {
                if (regs[i]->registerName == restoredRegister) {
                    model_.SelectAt(PopupModel::Group::Registers, i);
                    break;
                }
            }
        } else if (undoKind == clipp::UndoSlotKind::Activity && restoredItemID != 0) {
            const auto& history = model_.VisibleHistory();
            for (std::size_t i = 0; i < history.size(); ++i) {
                if (history[i]->historyId == restoredItemID) {
                    model_.SelectAt(PopupModel::Group::History, i);
                    break;
                }
            }
        }
        RenderList();  // RenderHighlight inside brings the selection into view
        FocusFilterBox();
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

    static void ActivityWatcher(const ClipboardActivityUpdate& update, void* userData) {
        auto* self = static_cast<PopupWindow*>(userData);
        if (self == nullptr || !self->dispatcher_) {
            return;
        }
        // Coarse but correct: any store change re-snapshots while visible.
        // Except mid-rename — a re-render would rebuild the editor row and
        // eat the user's typing; the commit/cancel path refreshes instead.
        const auto type = update.type;
        const uint64_t itemID = update.itemID;
        self->dispatcher_.TryEnqueue([self, type, itemID]() {
            if (self->hwnd_ != nullptr && IsWindowVisible(self->hwnd_)
                && !self->editingRegister_.has_value()) {
                self->RebuildFromStores();
                self->UpdateColumnLayout();
                // Pragmatic rule (user-ratified, same as ClippPage): a new or
                // relocated item IS the new current clipboard — select it, and
                // RenderList's highlight pass brings it into view (it sits at
                // the top). Only in an unfiltered browse: mid-search the item
                // may not even be visible, and yanking the selection would
                // fight what the user is doing.
                if ((type == ClipboardActivityUpdate::Type::Added
                     || type == ClipboardActivityUpdate::Type::Moved)
                    && self->model_.Filter().empty()) {
                    const auto& history = self->model_.VisibleHistory();
                    for (std::size_t i = 0; i < history.size(); ++i) {
                        if (history[i]->historyId == itemID) {
                            self->model_.SelectAt(PopupModel::Group::History, i);
                            break;
                        }
                    }
                }
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
        case WM_MOUSEACTIVATE:
            // Belt to the WS_EX_NOACTIVATE suspenders: clicks never activate.
            return MA_NOACTIVATE;
        case kHookDismissMessage:
            // Light dismiss, posted by the LL mouse hook (click landed on a
            // foreign window) or the foreground watcher (activation moved to a
            // foreign window). Replaces the old WM_ACTIVATE loss signal, which
            // a never-activated window cannot receive.
            Dismiss();
            return 0;
        case WM_ACTIVATE:
            // Vestigial under WS_EX_NOACTIVATE, kept as a backstop: if
            // something manages to deactivate us toward a foreign thread,
            // treat it as the old light dismiss.
            if (LOWORD(wParam) == WA_INACTIVE) {
                const HWND other = reinterpret_cast<HWND>(lParam);
                const DWORD ourThread = GetCurrentThreadId();
                if (other == nullptr ||
                    GetWindowThreadProcessId(other, nullptr) != ourThread) {
                    Dismiss();
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
        case WM_TIMER:
            if (wParam == kPasteTimerId) {
                OnPasteTimer();
                return 0;
            }
            if (wParam == kTypeArmTimerId) {
                // The confirming click never came; stand down quietly.
                DisarmType();
                HideTypeBubble();
                return 0;
            }
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
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
            Dismiss();
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
    Button pasteButton_{ nullptr };
    Button renameButton_{ nullptr };
    Button privateButton_{ nullptr };
    Button deleteButton_{ nullptr };
    Button undoButton_{ nullptr };
    Button typeButton_{ nullptr };
    Grid contentRoot_{ nullptr };
    // Confirm/error bubble anchored under the Type button, and the arm state
    // that makes a risky run take a second, deliberate click.
    StackPanel typeBubble_{ nullptr };
    TextBlock typeBubbleText_{ nullptr };
    Border typeBubblePointer_{ nullptr };
    bool typeArmed_ = false;
    std::wstring typeArmedKey_;
    clipp::TypeSchedule pendingTypeSchedule_;
    bool pendingIsType_ = false;
    TextBox nameEditor_{ nullptr };
    bool previewUpdatePending_ = false;
    ToastWindow toastWindow_;
    PreviewWindow previewWindow_;
    std::vector<Border> rowBorders_;
    std::vector<Border> registerRowBorders_;
    std::unordered_map<uint64_t, ClipboardActivityDisplayItem> displayCache_;
    std::map<std::string, RegisterRowInfo> registerCache_;
    // Private registers the user peeked this popup session (name-keyed — the
    // register world has no item IDs). Cleared on Dismiss, like item peeks.
    std::set<std::string> peekedRegisterNames_;
    std::optional<std::string> editingRegister_;
    bool registersPresent_ = false;
    // Pending paste keystroke: armed at dismissal, fired once the target
    // regains the foreground (kPasteTimerId polls).
    static constexpr UINT_PTR kPasteTimerId = 1;
    // Disarms the Type confirmation if the second click never comes.
    static constexpr UINT_PTR kTypeArmTimerId = 2;
    HWND pasteTargetWindow_ = nullptr;
    int pasteRetriesLeft_ = 0;
    int pasteFocusStreak_ = 0;
    // Which OS theme the window (and every baked brush) was built for; a
    // mismatch at Summon rebuilds the whole window.
    bool builtForDark_ = false;
    PopupModel model_;
    std::size_t watcherID_ = 0;
};

std::unique_ptr<PopupWindow> g_popupWindow;

}  // namespace

namespace clipp {

// The three public entry points all run inside the tray window's message
// pump (hotkey, menu command, WM_DESTROY). A WinRT exception escaping any of
// them reaches a COM boundary and fail-fasts the process with a stowed
// exception (0xC000027B) — invisible to crash handlers. The popup is a
// convenience surface: losing it is always better than taking the app down,
// so every boundary swallows-and-logs instead.
void TogglePopupWindow() {
    try {
        if (!g_popupWindow) {
            g_popupWindow = std::make_unique<PopupWindow>();
        }
        g_popupWindow->Toggle();
    } catch (const winrt::hresult_error& e) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Popup toggle failed (0x%08X): %ls", static_cast<uint32_t>(e.code()),
            e.message().c_str());
    } catch (...) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Popup toggle failed.");
    }
}

bool PopupPreTranslateMessage(MSG* msg) {
    try {
        return g_popupWindow ? g_popupWindow->PreTranslateMessage(msg) : false;
    } catch (...) {
        return false;  // an unrouted key is nothing; a dead process is something
    }
}

void DestroyPopupWindow() {
    try {
        if (g_popupWindow) {
            g_popupWindow->Destroy();
        }
    } catch (...) {
    }
    g_popupWindow.reset();  // released even if teardown misbehaved
}

}  // namespace clipp
