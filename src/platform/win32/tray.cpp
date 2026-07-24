// platform.h first: it brings WinSock2.h before Windows.h can pull the legacy
// winsock.h (redefinition soup otherwise), and declares the utf8/utf16
// converters utils.h depends on.
#include "platform.h"

#include <Windows.h>
#include <shellapi.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <cwchar>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "Logger.h"
#include "AutoStart.h"
#include "ClipboardFlowUi.h"
#include "Settings.h"
#include "resource.h"
#include "utils.h"
#include "PopupWindow.h"
#include "xaml_dialog.h"
#include "platform/uistrings.h"
#include "version.h"
#include "clipp-win32-darkmode32/DMSubclass.h"
#pragma comment(lib, "darkmode32.lib")

#ifdef _UNICODE
    #if defined _M_IX86
        #pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
    #elif defined _M_X64
        #pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
    #else
        #pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
    #endif
#endif

// Custom message ID for our tray icon events
#define WM_TRAYICON (WM_USER + 1)
// Global summon hotkey for the visual-paste popup (RegisterHotKey id).
#define HOTKEY_ID_POPUP 1
// Posted by the clipboard-flow handler (network/clipboard threads) to run the
// icon nudge on the tray thread. wParam = ClipboardFlowDirection.
#define WM_CLIPFLOW (WM_USER + 2)
#define ID_TRAY_OPEN  1000
#define ID_TRAY_ABOUT 1001
#define ID_TRAY_EXIT  1002
#define ID_TRAY_POPUP 1003
#define IDT_NUDGE 1

static constexpr wchar_t kTrayWindowClassName[] = L"ClippHiddenTrayWindow";
static constexpr wchar_t kShowMainWindowMessageName[] = L"ClippShowMainWindow";
static const UINT g_showMainWindowMessage = RegisterWindowMessageW(kShowMainWindowMessageName);
// Broadcast by the shell to all top-level windows when the taskbar / notification area is
// (re)created -- e.g. after Explorer crashes or is restarted, which drops all tray icons.
static const UINT g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

extern Settings g_settings;

// Global handle for cleanup
static NOTIFYICONDATAW g_nid = {};
static HWND g_trayWindow = NULL;

// ---- Clipboard-flow feedback (icon nudge + last-event tooltip) --------------
// The registered handler runs on network/clipboard threads: it records the
// event under g_flowMutex and posts WM_CLIPFLOW; everything visual happens on
// the tray thread. The nudge is the base icon re-blitted at a vertical pixel
// offset (up = sent, down = received), one pass, then back to the base icon —
// no residual state; the tooltip carries "what happened recently" instead.
static std::mutex g_flowMutex;
static bool g_flowValid = false;
static clipp::ClipboardFlowDirection g_flowDirection = clipp::ClipboardFlowDirection::Sent;
static std::wstring g_flowPeerName;
static std::chrono::steady_clock::time_point g_flowWhen{};

// Travel magnitudes in thousandths of the icon height — the icon is 16px only
// at 96 DPI now (PMv2: 24 at 150%, 32 at 200%). At 16px this reproduces the
// original {1,2,1}-pixel curve; larger icons get proportional (slightly
// up-rounded) travel instead of a sub-pixel shimmy. Sign = direction.
static constexpr int  kNudgeTravelPermille[] = { 80, 150, 80 };
static constexpr UINT kNudgeFrameMs = 90;
static HICON g_nudgeFrames[2][3] = {};  // [Sent][frame], [Received][frame]
static int   g_nudgeFrameIndex = -1;    // -1 = idle

// Re-blit `base`'s pixels shifted by rowDelta rows (dst[y] = src[y + rowDelta],
// so positive = content moves UP) into a fresh icon. Works on the raw 32bpp
// pixels so per-pixel alpha survives — DrawIconEx onto a DIB scrambles the
// alpha channel on some GDI paths.
static HICON ComposeOffsetIcon(HICON base, int rowDelta) {
    ICONINFO info{};
    if (!GetIconInfo(base, &info)) {
        return nullptr;
    }
    HICON result = nullptr;
    BITMAP bm{};
    if (info.hbmColor != nullptr &&
        GetObjectW(info.hbmColor, sizeof(bm), &bm) != 0 && bm.bmWidth > 0 && bm.bmHeight > 0) {
        const int width = bm.bmWidth;
        const int height = bm.bmHeight;
        const size_t stride = static_cast<size_t>(width) * 4;
        std::vector<unsigned char> src(stride * height);
        std::vector<unsigned char> dst(stride * height, 0);

        BITMAPINFO query{};
        query.bmiHeader.biSize = sizeof(query.bmiHeader);
        query.bmiHeader.biWidth = width;
        query.bmiHeader.biHeight = -height;  // top-down
        query.bmiHeader.biPlanes = 1;
        query.bmiHeader.biBitCount = 32;
        query.bmiHeader.biCompression = BI_RGB;

        HDC screenDc = GetDC(nullptr);
        if (screenDc != nullptr &&
            GetDIBits(screenDc, info.hbmColor, 0, height, src.data(), &query, DIB_RGB_COLORS) == height) {
            for (int y = 0; y < height; ++y) {
                const int from = y + rowDelta;
                if (from >= 0 && from < height) {
                    std::memcpy(&dst[y * stride], &src[from * stride], stride);
                }
            }

            BITMAPINFO create{};
            create.bmiHeader = query.bmiHeader;
            create.bmiHeader.biHeight = -height;
            void* bits = nullptr;
            HBITMAP color = CreateDIBSection(screenDc, &create, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (color != nullptr && bits != nullptr) {
                std::memcpy(bits, dst.data(), dst.size());
                // All-zero mono mask: 32bpp icons with alpha ignore it, and the
                // shipped clipp.ico carries proper alpha at tray sizes.
                HBITMAP mask = CreateBitmap(width, height, 1, 1, nullptr);
                if (mask != nullptr) {
                    ICONINFO out{};
                    out.fIcon = TRUE;
                    out.hbmColor = color;
                    out.hbmMask = mask;
                    result = CreateIconIndirect(&out);
                    DeleteObject(mask);
                }
            }
            if (color != nullptr) {
                DeleteObject(color);
            }
        }
        if (screenDc != nullptr) {
            ReleaseDC(nullptr, screenDc);
        }
    }
    if (info.hbmColor != nullptr) DeleteObject(info.hbmColor);
    if (info.hbmMask != nullptr) DeleteObject(info.hbmMask);
    return result;
}

static void EnsureNudgeFrames() {
    if (g_nudgeFrames[0][0] != nullptr || g_nid.hIcon == nullptr) {
        return;
    }
    // The authored curve rides the real icon height (LoadIconMetric hands us
    // the DPI-scaled size).
    int height = 16;
    ICONINFO info{};
    if (GetIconInfo(g_nid.hIcon, &info)) {
        BITMAP bm{};
        if (info.hbmColor != nullptr &&
            GetObjectW(info.hbmColor, sizeof(bm), &bm) != 0 && bm.bmHeight > 0) {
            height = bm.bmHeight;
        }
        if (info.hbmColor != nullptr) DeleteObject(info.hbmColor);
        if (info.hbmMask != nullptr) DeleteObject(info.hbmMask);
    }
    for (size_t i = 0; i < 3; ++i) {
        const int scaled = MulDiv(height, kNudgeTravelPermille[i], 1000);
        const int offset = scaled < 1 ? 1 : scaled;
        // Screen y grows downward, and ComposeOffsetIcon's positive rowDelta
        // moves content up — so Sent gets +offset (up), Received −offset (down).
        g_nudgeFrames[0][i] = ComposeOffsetIcon(g_nid.hIcon, offset);
        g_nudgeFrames[1][i] = ComposeOffsetIcon(g_nid.hIcon, -offset);
    }
}

// NIM_MODIFY through a local copy so g_nid.hIcon permanently holds the base
// icon (the TaskbarCreated re-add path reads it).
static void ShowTrayIconFrame(HICON icon) {
    if (icon == nullptr) {
        return;
    }
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_ICON;
    nid.hIcon = icon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void StartNudge(HWND hwnd, clipp::ClipboardFlowDirection direction) {
    // Setting gates the MOTION only: the event was already recorded for the
    // hover tooltip before this was posted.
    if (!g_settings.animateFlowFeedback()) {
        return;
    }
    EnsureNudgeFrames();
    const int dir = (direction == clipp::ClipboardFlowDirection::Received) ? 1 : 0;
    if (g_nudgeFrames[dir][0] == nullptr) {
        return;  // composition failed; skip the animation, tooltip still updates
    }
    // A new event mid-animation restarts the pass (coalescing: a burst of
    // copies re-nudges instead of strobing through queued passes).
    g_nudgeFrameIndex = 0;
    ShowTrayIconFrame(g_nudgeFrames[dir][0]);
    // Subsequent WM_TIMER ticks read the direction back from g_flow*.
    SetTimer(hwnd, IDT_NUDGE, kNudgeFrameMs, nullptr);
}

static void AdvanceNudge(HWND hwnd) {
    int dir = 0;
    {
        std::lock_guard<std::mutex> lock(g_flowMutex);
        dir = (g_flowDirection == clipp::ClipboardFlowDirection::Received) ? 1 : 0;
    }
    ++g_nudgeFrameIndex;
    if (g_nudgeFrameIndex >= 0 && g_nudgeFrameIndex < 3 && g_nudgeFrames[dir][g_nudgeFrameIndex] != nullptr) {
        ShowTrayIconFrame(g_nudgeFrames[dir][g_nudgeFrameIndex]);
        return;
    }
    KillTimer(hwnd, IDT_NUDGE);
    g_nudgeFrameIndex = -1;
    ShowTrayIconFrame(g_nid.hIcon);  // back to the byte-identical idle state
}

// Rebuild the tooltip just-in-time on hover, so the relative age ("12s ago")
// is computed at read time and never goes stale. Rate-limited by only calling
// NIM_MODIFY when the rendered text actually changed (the age string ticks at
// most once a second; hover mouse-moves arrive far more often).
static void UpdateTrayTooltip() {
    std::wstring tip = CLP_W(CLP_UI_STATUS_TOOLTIP);
    {
        std::lock_guard<std::mutex> lock(g_flowMutex);
        if (g_flowValid) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - g_flowWhen).count();
            const std::wstring age = Utf8ToWideString(
                clipp::FormatRelativeAgeUtf8(elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0));
            if (g_flowDirection == clipp::ClipboardFlowDirection::Received) {
                tip += L"\nReceived from " + g_flowPeerName + L" " + age;
            } else {
                tip += L"\nSent " + age;
            }
        }
    }
    static std::wstring lastSetTip;
    if (tip == lastSetTip) {
        return;
    }
    lastSetTip = tip;
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void DestroyNudgeFrames() {
    for (auto& perDirection : g_nudgeFrames) {
        for (HICON& frame : perDirection) {
            if (frame != nullptr) {
                DestroyIcon(frame);
                frame = nullptr;
            }
        }
    }
}

// The core event callback for the hidden window
LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_showMainWindowMessage != 0 && msg == g_showMainWindowMessage) {
        ShowClippMainDialog(hwnd);
        return 0;
    }

    if (g_taskbarCreatedMessage != 0 && msg == g_taskbarCreatedMessage) {
        // Explorer restarted and recreated the notification area, dropping our icon.
        // Re-add it (g_nid is fully populated once TrayIconMessageLoop ran the initial NIM_ADD).
        if (g_nid.hWnd != nullptr) {
            Shell_NotifyIconW(NIM_ADD, &g_nid);
        }
        return 0;
    }

    switch (msg) {
        case WM_CREATE:
            DarkMode::setWindowEraseBgSubclass(hwnd);
            DarkMode::setDarkWndNotifySafe(hwnd, true);
			break;
        case WM_CLIPFLOW:
            StartNudge(hwnd, static_cast<clipp::ClipboardFlowDirection>(wParam));
            break;

        case WM_HOTKEY:
            if (wParam == HOTKEY_ID_POPUP) {
                clipp::TogglePopupWindow();
            }
            break;

        case WM_TIMER:
            if (wParam == IDT_NUDGE) {
                AdvanceNudge(hwnd);
            }
            break;

        case WM_TRAYICON:
            if (LOWORD(lParam) == WM_MOUSEMOVE) {
                // Hovering: refresh the tooltip just before the shell shows it.
                UpdateTrayTooltip();
            }
            // When the user right-clicks the tray icon...
            if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                ShowClippMainDialog(hwnd);
            }
            else if (LOWORD(lParam) == WM_RBUTTONUP) {
                // This window is message-only, so it never receives the WM_SETTINGCHANGE
                // broadcast that refreshes the theme. Re-derive from the registry (which
                // also re-flushes menu themes) so the popup matches the current OS theme.
                DarkMode::setDarkModeConfig();

                // 1. Create a native, Unicode context menu
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, CLP_W(CLP_UI_OPEN_CLIPP));
                SetMenuDefaultItem(hMenu, ID_TRAY_OPEN, FALSE);
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_POPUP, CLP_W(CLP_UI_TRAY_POPUP));
                AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_ABOUT, CLP_W(CLP_UI_ABOUT_CLIPP));
                AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, CLP_W(CLP_UI_EXIT_CLIPP));

                // 2. Windows bug workaround: You must bring the hidden window
                // to the foreground before showing the menu, or it won't disappear
                // when the user clicks away.
                SetForegroundWindow(hwnd);

                // 3. Get the mouse position to spawn the menu exactly on the cursor
                POINT pt;
                GetCursorPos(&pt);
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            break;

        case WM_COMMAND:
            // Handle the menu clicks
            switch (LOWORD(wParam)) {
            case ID_TRAY_OPEN:
                ShowClippMainDialog(hwnd);
                break;
            case ID_TRAY_POPUP:
                clipp::TogglePopupWindow();
                break;
            case ID_TRAY_ABOUT:
                DarkMode::DarkMessageBox(
                    g_trayWindow,
                    CLP_W(CLP_UI_ABOUT_TITLE) L" v" CLP_W(CLIPP_VERSION_STRING_3PART) L"\n"
                    CLP_W(CLP_UI_TAGLINE) L"\n\n"
                    CLP_W(CLP_UI_COPYRIGHT) L"\n"
                    CLP_W(CLP_UI_MIT_LICENSE) L"\n\n"
                    L"Uses open source libraries including libsodium, xxHash, Zstandard, C++/WinRT, and darkmode32plus.",
                    CLP_W(CLP_UI_ABOUT_CLIPP),
                    MB_ICONINFORMATION | MB_OK);
                break;
            case ID_TRAY_EXIT:
                UnregisterClippAutoStart();
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                break;
            }
            break;

        case WM_CLOSE:
		    DestroyWindow(hwnd);
		    break;

        case WM_DESTROY:
            clipp::SetClipboardFlowHandler(nullptr);
            UnregisterHotKey(hwnd, HOTKEY_ID_POPUP);
            clipp::DestroyPopupWindow();
            KillTimer(hwnd, IDT_NUDGE);
            DestroyNudgeFrames();
            CloseClippMainDialog();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            g_trayWindow = NULL;
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

void TrayIconMessageLoop(bool showNetworkPageOnStartup) {
    // darkmode32 is built with _DARKMODE_NO_INI_CONFIG, so initDarkMode() derives the mode
    // from the registry (AppsUseLightTheme -> follows the OS) instead of defaulting to
    // force-dark. isEnabled() then reflects the real OS theme, which drives the title bar,
    // tray menu, and the XAML island's RequestedTheme.
    DarkMode::initDarkMode();
    HINSTANCE hInstance = GetModuleHandleW(NULL);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kTrayWindowClassName;
    RegisterClassW(&wc);

    // Top-level (not HWND_MESSAGE) so it receives the "TaskbarCreated" broadcast (and
    // WM_SETTINGCHANGE) -- message-only windows get neither. WS_EX_TOOLWINDOW + never being
    // shown keeps it out of the taskbar and Alt+Tab.
    g_trayWindow = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);

    // Visual-paste popup summon hotkey: Win+Insert (probe-verified free), with
    // Win+Alt+V as the fallback when something else owns it. MOD_NOREPEAT so a
    // held chord doesn't machine-gun the toggle. Failure is non-fatal — the
    // tray menu keeps the popup reachable.
    if (RegisterHotKey(g_trayWindow, HOTKEY_ID_POPUP, MOD_WIN | MOD_NOREPEAT, VK_INSERT)) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, L"Popup hotkey registered: Win+Insert.");
    } else if (RegisterHotKey(g_trayWindow, HOTKEY_ID_POPUP, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'V')) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, L"Win+Insert unavailable; popup hotkey is Win+Alt+V.");
    } else {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"No popup hotkey could be registered; the tray menu still opens it.");
    }

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_trayWindow;
    g_nid.uID = 1; // Arbitrary ID for this app's icon
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // LoadIconMetric, NOT LoadImage(LR_SHARED): the shared cache returns
    // whatever size was loaded for this resource FIRST — the dialog class's
    // 32x32 LoadIconW here — and the shell then rescales it into a blurry
    // tray icon. LoadIconMetric always decodes the .ico frame matching the
    // small-icon metric at the current DPI (the file carries 16/20/24/...).
    if (FAILED(LoadIconMetric(hInstance, MAKEINTRESOURCEW(IDI_CLIPP_ICON),
                              LIM_SMALL, &g_nid.hIcon))
        || g_nid.hIcon == nullptr) {
        g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }

    wcscpy_s(g_nid.szTip, CLP_W(CLP_UI_STATUS_TOOLTIP));

    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Ambient send/receive feedback: the handler runs on network/clipboard
    // threads, so it just records the event and bounces to this thread.
    clipp::SetClipboardFlowHandler([](clipp::ClipboardFlowDirection direction, const std::string& peerNameUtf8) {
        {
            std::lock_guard<std::mutex> lock(g_flowMutex);
            g_flowValid = true;
            g_flowDirection = direction;
            g_flowPeerName = Utf8ToWideString(peerNameUtf8);
            g_flowWhen = std::chrono::steady_clock::now();
        }
        if (g_trayWindow != NULL) {
            PostMessageW(g_trayWindow, WM_CLIPFLOW, static_cast<WPARAM>(direction), 0);
        }
    });

    if (showNetworkPageOnStartup) {
        ShowClippMainDialog(g_trayWindow, ClippMainDialogPage::Network);
    }

    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Unicode Tray Icon initialized. Entering message loop.");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        // Popup first: the main dialog object stays alive (hidden) after its
        // first open, and its island's PreTranslateMessage can claim input
        // meant for the popup's island. The popup hook self-gates on
        // visibility, so this order costs the dialog nothing.
        if (clipp::PopupPreTranslateMessage(&msg)) {
            continue;
        }
        if (ClippMainDialogPreTranslateMessage(&msg)) {
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void TrayIconShutdown() {
    if (g_trayWindow != NULL) {
        PostMessageW(g_trayWindow, WM_CLOSE, 0, 0);
    }
}
