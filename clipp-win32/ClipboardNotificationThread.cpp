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

ClipboardPayload ReadClipboardData(HWND hwnd) {
    ClipboardPayload payload{};
    payload.formatId = 0; // 0 indicates empty/unsupported

    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(hwnd)) {
            // 1. Try to read Text
            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    const wchar_t* utf16Str = static_cast<const wchar_t*>(GlobalLock(hData));
                    if (utf16Str) {
                        // Calculate required buffer size for UTF-8 (including null terminator)
                        int utf8Size = WideCharToMultiByte(CP_UTF8, 0, utf16Str, -1, nullptr, 0, nullptr, nullptr);
                        if (utf8Size > 0) {
                            payload.formatId = CF_UNICODETEXT;
                            payload.rawData.resize(utf8Size);
                            // Perform the actual conversion straight into the vector
                            WideCharToMultiByte(CP_UTF8, 0, utf16Str, -1,
                                reinterpret_cast<char*>(payload.rawData.data()), utf8Size, nullptr, nullptr);
                        }
                        GlobalUnlock(hData);
                    }
                }
            }
            // 2. Try to read an Image (if text isn't available)
            else if (IsClipboardFormatAvailable(CF_DIB)) {
                HANDLE hData = GetClipboardData(CF_DIB);
                if (hData) {
                    const unsigned char* dibData = static_cast<const unsigned char*>(GlobalLock(hData));
                    if (dibData) {
                        // GlobalSize tells us exactly how many bytes the DIB takes up in memory
                        SIZE_T dataSize = GlobalSize(hData);
                        if (dataSize > 0) {
                            payload.formatId = CF_DIB;
                            payload.rawData.assign(dibData, dibData + dataSize);
                        }
                        GlobalUnlock(hData);
                    }
                }
            }
            CloseClipboard();
            break;
        }
        // Yield and wait. 
        Sleep(10 + (i * 10));
    }

    return payload;
}

void SetClipboardData(const ClipboardPayload& payload) {
    // TODO
}
