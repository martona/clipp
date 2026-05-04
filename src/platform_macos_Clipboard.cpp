#include "Clipboard.h"
#include <thread>
#include <future>
#include <cstring>
#include "Logger.h"

static std::thread g_clipboardThread;
static PlatformWindowHandle g_hwnd = nullptr;

#define CLIPBOARD_DEBOUNCE_TIMER_ID 1
#define CLIPBOARD_DEBOUNCE_INTERVAL_MS 250

static ClipboardCallback g_clipboardCallback = nullptr;

PlatformWindowHandle CreateClipboardNotificationWindow(ClipboardCallback cb) {
    return nullptr;
}

static void ClipboardThreadProc(std::promise<bool> initPromise, ClipboardCallback callback) {
    //TODO
}

bool StartClipboardNotification(ClipboardCallback callback) {
    //TODO
    return true;
}

void StopClipboardNotification() {
    //TODO
}

ClipboardPayload ReadClipboardData(PlatformWindowHandle hwnd) {
    ClipboardPayload payload{};
    payload.formatId = 0; // 0 indicates empty/unsupported
    //TODO
    return payload;
}

void SetClipboardData(const ClipboardPayload& payload) {
    //TODO
}
