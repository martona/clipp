#include "Clipboard.h"
#include "Logger.h"

#import <AppKit/AppKit.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <future>
#include "ClipboardHashGuard.h"

static std::thread g_clipboardThread;
static std::atomic<bool> g_stopClipboardThread{false};
static std::atomic<NSInteger> g_lastChangeCount{0};
static ClipboardHashGuard g_clipboardHashGuard;

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
    payload.meta.formatId = CLIPP_FORMAT_NONE;

    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];

        // Try to read Text
        NSString* text = [pb stringForType:NSPasteboardTypeString];
        if (text) {
            NSData* data = [text dataUsingEncoding:NSUTF8StringEncoding];
            if (data) {
                payload.meta.formatId = CLIPP_FORMAT_UTF8;
                const unsigned char* bytes = static_cast<const unsigned char*>([data bytes]);
                payload.rawData.assign(bytes, bytes + [data length]);
                payload.rawData.push_back('\0');
            }
        }

        if (payload.meta.formatId == CLIPP_FORMAT_NONE) {
            NSData* imageData = [pb dataForType:@"public.jpeg"];
            if (imageData != nil && [imageData length] > 0) {
                payload.meta.formatId = CLIPP_FORMAT_JPEG;
                const unsigned char* bytes = static_cast<const unsigned char*>([imageData bytes]);
                payload.rawData.assign(bytes, bytes + [imageData length]);
                g_logger.log(__FUNCTION__, Logger::Level::Info, L"Read JPEG image from system clipboard (JPEG payload: %zu bytes)", payload.rawData.size());
            }
            else {
                imageData = [pb dataForType:NSPasteboardTypePNG];
                if (imageData != nil && [imageData length] > 0) {
                    payload.meta.formatId = CLIPP_FORMAT_PNG;
                    const unsigned char* bytes = static_cast<const unsigned char*>([imageData bytes]);
                    payload.rawData.assign(bytes, bytes + [imageData length]);
                    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Read PNG image from system clipboard (PNG payload: %zu bytes)", payload.rawData.size());
                }
            }
        }
    }

    if (payload.meta.formatId != CLIPP_FORMAT_NONE) {
        if (!g_clipboardHashGuard.AcceptCurrent(payload)) {
            g_logger.log(__FUNCTION__, Logger::Level::Debug, "Ignoring clipboard notification for already-current clipboard contents.");
            payload.meta.formatId = CLIPP_FORMAT_NONE;
            payload.rawData.clear();
        }
    }

    return payload;
}

bool IsClipboardDataCurrent(const ClipboardPayload& payload) {
    return payload.meta.formatId != CLIPP_FORMAT_NONE && g_clipboardHashGuard.IsCurrent(payload);
}

void SetClipboardData(
    ClipboardPayload& payload,
    bool markAsClippOriginated,
    std::shared_ptr<const ClipboardPayload> delayedRenderPayloadReference) {
    (void)delayedRenderPayloadReference;

    if (markAsClippOriginated && g_clipboardHashGuard.IsCurrent(payload)) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Clipboard contents already current; not setting clipboard data");
        return;
    }

    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];

        bool wroteClipboard = false;

        if (payload.meta.formatId == CLIPP_FORMAT_UTF8) {
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
        } else if (IsClippImageFormat(payload.meta.formatId)) {
            NSString* pasteboardType = payload.meta.formatId == CLIPP_FORMAT_JPEG ? @"public.jpeg" : NSPasteboardTypePNG;
            NSData* imageData = [NSData dataWithBytes:payload.rawData.data() length:payload.rawData.size()];
            wroteClipboard = [pb setData:imageData forType:pasteboardType];
            if (wroteClipboard) {
                g_logger.log(__FUNCTION__, Logger::Level::Info, L"Wrote %ls image to system clipboard (payload: %zu bytes)",
                             ClippClipboardFormatNameW(payload.meta.formatId),
                             payload.rawData.size());
            }
        }
        else {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Unsupported clipboard payload format %ls (%u); nothing written",
                         ClippClipboardFormatNameW(payload.meta.formatId),
                         payload.meta.formatId);
        }

        if (wroteClipboard && markAsClippOriginated) {
            // Fast-forward our known changeCount so we don't trigger a recursive network broadcast of our own change
            g_lastChangeCount.store([pb changeCount]);

            g_clipboardHashGuard.RememberCurrent(payload);
        }
        else {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"System clipboard write did not complete (format: %ls, ID: %u, payload size: %zu bytes)",
                         ClippClipboardFormatNameW(payload.meta.formatId),
                         payload.meta.formatId,
                         payload.rawData.size());
        }
    }
}
