#include "Clipboard.h"
#include <windows.h>
#include <thread>
#include <future>
#include <cstring>
#include <xxhash.h>
#include "Logger.h"

static std::thread g_clipboardThread;
static HWND g_hwnd = nullptr;
static std::mutex g_hashMutex;
static XXH128_hash_t g_lastClipboardHash{ 0, 0 };

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
            else {
                g_logger.log(__FUNCTION__, Logger::Level::Info, L"No supported clipboard format available");
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
    XXH128_hash_t newHash = XXH3_128bits(payload.rawData.data(), payload.rawData.size());
    {
        std::lock_guard<std::mutex> lock(g_hashMutex);
        if (newHash.high64 == g_lastClipboardHash.high64 && newHash.low64 == g_lastClipboardHash.low64) {
            g_logger.log(__FUNCTION__, Logger::Level::Info, L"Clipboard hash match, not setting clipboard data");
            return;
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(g_hwnd)) {
            if (!EmptyClipboard()) {
                CloseClipboard();
                Sleep(10 + (i * 10));
                continue;
            }

            if (payload.formatId == CF_UNICODETEXT) {
                const char* utf8Data = reinterpret_cast<const char*>(payload.rawData.data());
                const int utf8Bytes = static_cast<int>(payload.rawData.size());
                const int wideChars = MultiByteToWideChar(CP_UTF8, 0, utf8Data, utf8Bytes, nullptr, 0);
                if (wideChars > 0) {
                    const SIZE_T wideBytes = static_cast<SIZE_T>(wideChars) * sizeof(wchar_t);
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wideBytes);
                    if (hMem) {
                        wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hMem));
                        if (dst) {
                            if (MultiByteToWideChar(CP_UTF8, 0, utf8Data, utf8Bytes, dst, wideChars) > 0) {
                                GlobalUnlock(hMem);
                                if (!::SetClipboardData(CF_UNICODETEXT, hMem)) {
                                    GlobalFree(hMem);
                                }
                                else {
                                    std::lock_guard<std::mutex> lock(g_hashMutex);
                                    g_lastClipboardHash = newHash;
                                }
                            }
                            else {
                                GlobalUnlock(hMem);
                                GlobalFree(hMem);
                            }
                        }
                        else {
                            GlobalFree(hMem);
                        }
                    }
                }
            }
            else if (payload.formatId == CF_DIB) {
                const size_t bytes = payload.rawData.size();
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (hMem) {
                    void* dst = GlobalLock(hMem);
                    if (dst) {
                        std::memcpy(dst, payload.rawData.data(), bytes);
                        GlobalUnlock(hMem);
                        if (!::SetClipboardData(CF_DIB, hMem)) {
                            GlobalFree(hMem);
                        }
                        else {
                            std::lock_guard<std::mutex> lock(g_hashMutex);
                            g_lastClipboardHash = newHash;
                        }
                    }
                    else {
                        GlobalFree(hMem);
                    }
                }
            }

            CloseClipboard();
            break;
        }
        Sleep(10 + (i * 10));
    }
}
