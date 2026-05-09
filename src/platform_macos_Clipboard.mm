#include "Clipboard.h"
#include "Logger.h"

#import <AppKit/AppKit.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <future>
#include <cstring>
#include <xxhash.h>

static std::thread g_clipboardThread;
static std::atomic<bool> g_stopClipboardThread{false};
static std::atomic<NSInteger> g_lastChangeCount{0};

static std::mutex g_hashMutex;
static XXH128_hash_t g_lastClipboardHash{ 0, 0 };

static ClipboardCallback g_clipboardCallback = nullptr;

static bool IsPngStream(const std::vector<unsigned char>& data) {
    static constexpr unsigned char signature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    return data.size() >= sizeof(signature) && std::memcmp(data.data(), signature, sizeof(signature)) == 0;
}

PlatformWindowHandle CreateClipboardNotificationWindow(ClipboardCallback cb) {
    g_clipboardCallback = cb;
    return nullptr;
}

static void ClipboardThreadProc(std::promise<bool> initPromise, ClipboardCallback callback) {
    g_clipboardCallback = callback;
    g_stopClipboardThread.store(false);

    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        g_lastChangeCount.store([pb changeCount]);
    }

    initPromise.set_value(true);

    // Background polling loop to detect clipboard changes
    while (!g_stopClipboardThread.load()) {
        @autoreleasepool {
            NSPasteboard* pb = [NSPasteboard generalPasteboard];
            NSInteger currentCount = [pb changeCount];
            
            if (currentCount != g_lastChangeCount.load()) {
                g_lastChangeCount.store(currentCount);
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

        // Try to read PNG image data. Network image payloads are already PNG,
        // and macOS exposes PNG pasteboard data as public.png.
        NSData* pngData = [pb dataForType:NSPasteboardTypePNG];
        if (pngData) {
            payload.formatId = CF_DIB;
            const unsigned char* bytes = static_cast<const unsigned char*>([pngData bytes]);
            payload.rawData.assign(bytes, bytes + [pngData length]);
            if (IsPngStream(payload.rawData)) {
                g_logger.log(__FUNCTION__, Logger::Level::Info, L"Read PNG image from system clipboard (PNG payload: %zu bytes)", payload.rawData.size());
                return payload;
            }

            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Ignoring PNG pasteboard data with invalid PNG signature (%zu bytes)", payload.rawData.size());
            payload.formatId = 0;
            payload.rawData.clear();
        }
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

        bool wroteClipboard = false;

        if (payload.formatId == CF_UNICODETEXT) {
            size_t textLen = payload.rawData.size();
            if (textLen > 0 && payload.rawData.back() == '\0') {
                textLen--;
            }
            NSString* str = [[NSString alloc] initWithBytes:payload.rawData.data()
                                                   length:textLen
                                                   encoding:NSUTF8StringEncoding];
            if (str) {
                wroteClipboard = [pb setString:str forType:NSPasteboardTypeString];
            }
        } else if (payload.formatId == CF_DIB) {
            if (!IsPngStream(payload.rawData)) {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Refusing to write invalid PNG image payload to system clipboard (%zu bytes)", payload.rawData.size());
            }
            else {
                NSData* pngData = [NSData dataWithBytes:payload.rawData.data() length:payload.rawData.size()];
                wroteClipboard = [pb setData:pngData forType:NSPasteboardTypePNG];
                if (wroteClipboard) {
                    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Wrote PNG image to system clipboard (PNG payload: %zu bytes)", payload.rawData.size());
                }
            }
        }
        else {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Unsupported clipboard payload format ID %u; nothing written", payload.formatId);
        }

        if (wroteClipboard) {
            // Fast-forward our known changeCount so we don't trigger a recursive network broadcast of our own change
            g_lastChangeCount.store([pb changeCount]);

            std::lock_guard<std::mutex> lock(g_hashMutex);
            g_lastClipboardHash = newHash;
        }
        else {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"System clipboard write did not complete (format ID: %u, payload size: %zu bytes)", payload.formatId, payload.rawData.size());
        }
    }
}
