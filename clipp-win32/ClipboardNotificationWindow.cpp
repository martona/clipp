#include "ClipboardNotificationWindow.h"
#include <windows.h>

#define CLIPBOARD_DEBOUNCE_TIMER_ID 1
#define CLIPBOARD_DEBOUNCE_INTERVAL_MS 250

static ClipboardCallback g_clipboardCallback = nullptr;

LRESULT CALLBACK ClipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        // Register for clipboard update notifications
        AddClipboardFormatListener(hwnd);
        return 0;
    case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        PostQuitMessage(0);
        return 0;
    case WM_CLIPBOARDUPDATE:
        // Debounce: set (or reset) a one-shot timer for clipboard processing
        SetTimer(hwnd, CLIPBOARD_DEBOUNCE_TIMER_ID, CLIPBOARD_DEBOUNCE_INTERVAL_MS, NULL);
        return 0;
    case WM_TIMER:
        if (wParam == CLIPBOARD_DEBOUNCE_TIMER_ID) {
            KillTimer(hwnd, CLIPBOARD_DEBOUNCE_TIMER_ID);
            if (g_clipboardCallback) g_clipboardCallback(hwnd);
        }
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

HWND CreateClipboardNotificationWindow(ClipboardCallback cb) {
    g_clipboardCallback = cb;
    const wchar_t CLASS_NAME[] = L"ClipboardNotificationWindow";
    WNDCLASS wc = {};
    wc.lpfnWndProc = ClipboardWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);
    HWND hwnd = CreateWindowEx(
        0, CLASS_NAME, L"", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    // Hide the window
    ShowWindow(hwnd, SW_HIDE);
    return hwnd;
}
