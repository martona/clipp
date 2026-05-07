#include <Windows.h>
#include "Logger.h"
#include "clipp-win32-darkmode32/DMSubclass.h"
#pragma comment(lib, "darkmode32.lib")

// Custom message ID for our tray icon events
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_ABOUT 1001
#define ID_TRAY_EXIT  1002

// Global handle for cleanup
static NOTIFYICONDATAW g_nid = {};
static HWND g_trayWindow = NULL;

// The core event callback for the hidden window
LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            DarkMode::setWindowEraseBgSubclass(hwnd);
            DarkMode::setDarkWndNotifySafe(hwnd, true);
			break;
        case WM_TRAYICON:
            // When the user right-clicks the tray icon...
            if (LOWORD(lParam) == WM_RBUTTONUP) {
                // 1. Create a native, Unicode context menu
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_ABOUT, L"About Clipp");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

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
            case ID_TRAY_ABOUT:
                DarkMode::DarkMessageBox(NULL, L"Clipp v1.0\nSecure cross-platform clipboard sync.", L"About", MB_ICONINFORMATION | MB_OK);
                break;
            case ID_TRAY_EXIT:
                PostQuitMessage(0);
                break;
            }
            break;

        case WM_CLOSE:
		    PostQuitMessage(0);
		    break;

        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            break;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

void TrayIconMessageLoop() {
    DarkMode::initDarkMode();
    HINSTANCE hInstance = GetModuleHandleW(NULL);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ClippHiddenTrayWindow";
    RegisterClassW(&wc);

    g_trayWindow = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_trayWindow;
    g_nid.uID = 1; // Arbitrary ID for this app's icon
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);

    wcscpy_s(g_nid.szTip, L"Clipp Network Sync");

    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Unicode Tray Icon initialized. Entering message loop.");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void TrayIconShutdown() {
    if (g_trayWindow != NULL) {
        PostMessageW(g_trayWindow, WM_CLOSE, 0, 0);
    }
}
