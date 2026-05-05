#include "Clipboard.h"
#include "Logger.h"

#import <AppKit/AppKit.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <future>
#include <xxhash.h>

static std::thread g_clipboardThread;
static std::atomic<bool> g_stopClipboardThread{false};
static NSInteger g_lastChangeCount = 0;

static std::mutex g_hashMutex;
static XXH128_hash_t g_lastClipboardHash{ 0, 0 };

static ClipboardCallback g_clipboardCallback = nullptr;

PlatformWindowHandle CreateClipboardNotificationWindow(ClipboardCallback cb) {
    g_clipboardCallback = cb;
    return nullptr;
}

static void ClipboardThreadProc(std::promise<bool> initPromise, ClipboardCallback callback) {
    g_clipboardCallback = callback;
    g_stopClipboardThread.store(false);

    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        g_lastChangeCount = [pb changeCount];
    }

    initPromise.set_value(true);

    // Background polling loop to detect clipboard changes
    while (!g_stopClipboardThread.load()) {
        @autoreleasepool {
            NSPasteboard* pb = [NSPasteboard generalPasteboard];
            NSInteger currentCount = [pb changeCount];
            
            if (currentCount != g_lastChangeCount) {
                g_lastChangeCount = currentCount;
                if (g_clipboardCallback) {
                    g_clipboardCallback(nullptr);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

bool StartClipboardNotification(ClipboardCallback callback) {
    std::promise<bool> initPromise;
    std::future<bool> initFuture = initPromise.get_future();
    g_clipboardThread = std::thread(ClipboardThreadProc, std::move(initPromise), callback);
    return initFuture.get();
    return true;
}

void StopClipboardNotification() {
    g_stopClipboardThread.store(true);
    if (g_clipboardThread.joinable()) {
        g_clipboardThread.join();
    }
}

ClipboardPayload ReadClipboardData(PlatformWindowHandle hwnd) {
    ClipboardPayload payload{};
    payload.formatId = 0;

    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        
        // Try to read Text
        NSString* text = [pb stringForType:NSPasteboardTypeString];
        if (text) {
            NSData* data = [text dataUsingEncoding:NSUTF8StringEncoding];
            if (data) {
                payload.formatId = CF_UNICODETEXT;
                const unsigned char* bytes = static_cast<const unsigned char*>([data bytes]);
                payload.rawData.assign(bytes, bytes + [data length]);
                payload.rawData.push_back('\0');
                return payload;
            }
        }

        // TODO: handle image data
    }

    return payload;
}

void SetClipboardData(ClipboardPayload& payload) {
    XXH128_hash_t newHash = XXH3_128bits(payload.rawData.data(), payload.rawData.size());
    {
        std::lock_guard<std::mutex> lock(g_hashMutex);
        if (newHash.high64 == g_lastClipboardHash.high64 && newHash.low64 == g_lastClipboardHash.low64) {
            g_logger.log(__FUNCTION__, Logger::Level::Info, "Clipboard hash match, not setting clipboard data");
            return;
        }
    }

    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];

        if (payload.formatId == CF_UNICODETEXT) {
            size_t textLen = payload.rawData.size();
            if (textLen > 0 && payload.rawData.back() == '\0') {
                textLen--;
            }
            NSString* str = [[NSString alloc] initWithBytes:payload.rawData.data()
                                                   length:textLen
                                                   encoding:NSUTF8StringEncoding];
            if (str) {
                [pb setString:str forType:NSPasteboardTypeString];
            }
        } else if (payload.formatId == CF_DIB) {
            // TODO
        }

        // Fast-forward our known changeCount so we don't trigger a recursive network broadcast of our own change
        g_lastChangeCount = [pb changeCount];

        {
            std::lock_guard<std::mutex> lock(g_hashMutex);
            g_lastClipboardHash = newHash;
        }
    }
}
