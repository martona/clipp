#include "ClipboardNotificationThread.h"
#include "ClipboardNotificationWindow.h"
#include <thread>
#include <future>

static std::thread g_clipboardThread;
static HWND g_hwnd = nullptr;

static void ClipboardThreadProc(std::promise<bool> initPromise, ClipboardCallback callback) {
    g_hwnd = CreateClipboardNotificationWindow(callback);
    if (!g_hwnd) {
        initPromise.set_value(false);
        return;
    }
    initPromise.set_value(true);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

bool StartClipboardNotification(ClipboardCallback callback) {
    std::promise<bool> initPromise;
    std::future<bool> initFuture = initPromise.get_future();
    g_clipboardThread = std::thread(ClipboardThreadProc, std::move(initPromise), callback);
    if (!initFuture.get()) {
        if (g_clipboardThread.joinable())
            g_clipboardThread.join();
        return false;
    }
    return true;
}

void StopClipboardNotification() {
    if (g_hwnd)
        PostMessage(g_hwnd, WM_CLOSE, 0, 0);
    if (g_clipboardThread.joinable())
        g_clipboardThread.join();
}
