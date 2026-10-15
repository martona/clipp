#include "platform.h"

#import "../AppleShortcutsBridge.h"

#include "ClipboardActions.h"
#include "ClipboardActivityStore.h"
#include "ClipboardFormat.h"

namespace {
BOOL FailCopy(NSError** error, NSInteger code, NSString* message) {
    if (error != nullptr) {
        *error = [NSError errorWithDomain:@"net.clipp.shortcuts"
                                    code:code
                                userInfo:@{ NSLocalizedDescriptionKey: message }];
    }
    return NO;
}
}

@implementation CLPAppleShortcutsBridge

+ (BOOL)copyLatestItemWithError:(NSError**)error {
    // The AppIntent runs on the main actor in the menu bar app, after main.cpp
    // initializes the stores and networking. Snapshot enforces retention and
    // returns event-time order, including history replayed out of arrival order.
    const auto snapshot = g_clipboardActivityStore.Snapshot();
    if (snapshot.empty()) {
        return FailCopy(error, 1, @"No clipboard item is available to copy. Open Clipp and sync or copy an item first.");
    }
    const uint64_t itemID = snapshot.back().id;
    const auto payload = g_clipboardActivityStore.PayloadReference(itemID);
    if (!payload) {
        return FailCopy(error, 2, @"The latest clipboard item is no longer available.");
    }
    if (payload->EncodedBytes().empty()) {
        return FailCopy(error, 3, @"The latest clipboard item has no copyable content. It may have been marked private by its source app.");
    }
    if (IsClippTextFormat(payload->meta.formatId)) {
        const auto* bytes = payload->TryGetUncompressedBytes();
        if (bytes == nullptr) {
            return FailCopy(error, 4, @"The latest clipboard item's text could not be decoded.");
        }
        size_t length = bytes->size();
        if (length > 0 && bytes->back() == '\0') {
            --length;
        }
        NSString* text = [[NSString alloc] initWithBytes:bytes->data()
                                                length:length
                                              encoding:NSUTF8StringEncoding];
        if (text.length == 0) {
            return FailCopy(error, 4, @"The latest clipboard item has no copyable text.");
        }
    } else if (!IsClippImageFormat(payload->meta.formatId)) {
        return FailCopy(error, 5, @"The latest clipboard item's format is not supported.");
    }

    // Match Copy in Clipp: preserve event identity and privacy, force the local
    // pasteboard write, relocate the history item, and broadcast to peers.
    if (!clipp::ReshareActivityItem(itemID)) {
        return FailCopy(error, 2, @"The latest clipboard item is no longer available.");
    }
    return YES;
}

@end
