#include "Clipboard.h"
#include "Logger.h"

#import <AppKit/AppKit.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <future>
#include "ClipboardHashGuard.h"
#include "NetworkDefs.h"

// nspasteboard.org convention: an app marks pasteboard content as "do not put
// in clipboard history" by declaring this type alongside the actual content.
// The type's data is conventionally empty — the presence of the type is the
// signal. Apple's own NSPasteboardContentsCurrentHostOnly (set via
// prepareForNewContentsWithOptions:) has no read-side API, so on macOS this
// is the only practical detection signal.
static NSString* const kNspasteboardConcealedType = @"org.nspasteboard.ConcealedType";

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
    bool sourceMarkedPrivate = false;
    std::vector<unsigned char> bytes;

    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];

        sourceMarkedPrivate = [[pb types] containsObject:kNspasteboardConcealedType];
        if (sourceMarkedPrivate) {
            g_logger.log(__FUNCTION__, Logger::Level::Debug, "Source app marked clipboard content as private (org.nspasteboard.ConcealedType present).");
        }

        // Try to read Text
        NSString* text = [pb stringForType:NSPasteboardTypeString];
        if (text) {
            NSData* data = [text dataUsingEncoding:NSUTF8StringEncoding];
            if (data) {
                payload.meta.formatId = CLIPP_FORMAT_UTF8;
                const unsigned char* src = static_cast<const unsigned char*>([data bytes]);
                bytes.assign(src, src + [data length]);
                bytes.push_back('\0');
            }
        }

        if (payload.meta.formatId == CLIPP_FORMAT_NONE) {
            NSData* imageData = [pb dataForType:@"public.jpeg"];
            if (imageData != nil && [imageData length] > 0) {
                payload.meta.formatId = CLIPP_FORMAT_JPEG;
                const unsigned char* src = static_cast<const unsigned char*>([imageData bytes]);
                bytes.assign(src, src + [imageData length]);
                g_logger.log(__FUNCTION__, Logger::Level::Info, L"Read JPEG image from system clipboard (JPEG payload: %zu bytes)", bytes.size());
            }
            else {
                imageData = [pb dataForType:NSPasteboardTypePNG];
                if (imageData != nil && [imageData length] > 0) {
                    payload.meta.formatId = CLIPP_FORMAT_PNG;
                    const unsigned char* src = static_cast<const unsigned char*>([imageData bytes]);
                    bytes.assign(src, src + [imageData length]);
                    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Read PNG image from system clipboard (PNG payload: %zu bytes)", bytes.size());
                }
            }
        }
    }

    if (payload.meta.formatId == CLIPP_FORMAT_NONE) {
        return payload;
    }

    if (!payload.SetUncompressedBytes(std::move(bytes))) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, "Failed to encode clipboard payload; dropping.");
        payload.meta.formatId = CLIPP_FORMAT_NONE;
        return payload;
    }

    if (!g_clipboardHashGuard.AcceptCurrent(payload)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "Ignoring clipboard notification for already-current clipboard contents.");
        payload.meta.formatId = CLIPP_FORMAT_NONE;
        payload.SetEncodedBytes({});
        return payload;
    }

    if (sourceMarkedPrivate) {
        payload.meta.flags |= NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE;
    }

    return payload;
}

bool IsClipboardDataCurrent(const ClipboardPayload& payload) {
    return payload.meta.formatId != CLIPP_FORMAT_NONE && g_clipboardHashGuard.IsCurrent(payload);
}

void SetClipboardData(
    std::shared_ptr<const ClipboardPayload> payload,
    bool markAsClippOriginated) {
    if (!payload) {
        return;
    }

    if (markAsClippOriginated && g_clipboardHashGuard.IsCurrent(*payload)) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Clipboard contents already current; not setting clipboard data");
        return;
    }

    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];

        // When propagating a source-marked-private payload, ask the system to
        // keep this clipboard write off Universal Clipboard (host-only) and
        // declare org.nspasteboard.ConcealedType so other clipboard managers
        // / history tools skip it. Otherwise, plain clearContents.
        const bool sourceMarkedPrivate =
            (payload->meta.flags & NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE) != 0;
        if (sourceMarkedPrivate) {
            [pb prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
        } else {
            [pb clearContents];
        }

        bool wroteClipboard = false;

        if (payload->meta.formatId == CLIPP_FORMAT_UTF8) {
            const std::vector<unsigned char>* utf8 = payload->TryGetUncompressedBytes();
            if (utf8 == nullptr) {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, "Failed to obtain plaintext for clipboard text payload; nothing written.");
            } else {
                size_t textLen = utf8->size();
                if (textLen > 0 && utf8->back() == '\0') {
                    textLen--;
                }
                NSString* str = [[NSString alloc] initWithBytes:utf8->data()
                                                         length:textLen
                                                       encoding:NSUTF8StringEncoding];
                if (str) {
                    wroteClipboard = [pb setString:str forType:NSPasteboardTypeString];
                }
            }
        } else if (IsClippImageFormat(payload->meta.formatId)) {
            // Images are never zstd-compressed; EncodedBytes() is the image data.
            NSString* pasteboardType = payload->meta.formatId == CLIPP_FORMAT_JPEG ? @"public.jpeg" : NSPasteboardTypePNG;
            const std::vector<unsigned char>& imageBytes = payload->EncodedBytes();
            NSData* imageData = [NSData dataWithBytes:imageBytes.data() length:imageBytes.size()];
            wroteClipboard = [pb setData:imageData forType:pasteboardType];
            if (wroteClipboard) {
                g_logger.log(__FUNCTION__, Logger::Level::Info, L"Wrote %ls image to system clipboard (payload: %zu bytes)",
                             ClippClipboardFormatNameW(payload->meta.formatId),
                             imageBytes.size());
            }
        }
        else {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Unsupported clipboard payload format %ls (%u); nothing written",
                         ClippClipboardFormatNameW(payload->meta.formatId),
                         payload->meta.formatId);
        }

        if (wroteClipboard && sourceMarkedPrivate) {
            // Declare the conceal type so third-party clipboard managers and
            // history tools that respect the nspasteboard.org convention skip
            // this entry. Data is intentionally empty — presence of the type
            // is the signal.
            [pb setData:[NSData data] forType:kNspasteboardConcealedType];
        }

        if (wroteClipboard && markAsClippOriginated) {
            // Fast-forward our known changeCount so we don't trigger a recursive network broadcast of our own change
            g_lastChangeCount.store([pb changeCount]);

            g_clipboardHashGuard.RememberCurrent(*payload);
        }
        else {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"System clipboard write did not complete (format: %ls, ID: %u, payload size: %zu bytes)",
                         ClippClipboardFormatNameW(payload->meta.formatId),
                         payload->meta.formatId,
                         payload->EncodedBytes().size());
        }
    }
}
