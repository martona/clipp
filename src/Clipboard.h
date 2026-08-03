#pragma once
#include "platform.h"
#include "ClipboardPayload.h"

#include <memory>

// Creates an invisible window for clipboard notifications
using ClipboardCallback = void(*)(PlatformWindowHandle);

// Creates an invisible window for clipboard notifications, with callback
PlatformWindowHandle CreateClipboardNotificationWindow(ClipboardCallback cb);

// Starts the clipboard notification thread. Returns true on success.
bool StartClipboardNotification(ClipboardCallback callback);

// Stops the clipboard notification thread. Blocks until the thread exits.
void StopClipboardNotification();

// Reads the clipboard data and returns it as packet. The returned payload is
// fully populated via SetUncompressedBytes — hash is computed, compressed iff
// profitable. Format CLIPP_FORMAT_NONE means "nothing to send" (empty or echo).
ClipboardPayload ReadClipboardData(PlatformWindowHandle hwnd);
bool IsClipboardDataCurrent(const ClipboardPayload& payload);
// Writes the payload to the OS clipboard. The shared_ptr doubles as the
// delayed-render reference (Win32 needs it alive across CF_DIB rendering).
//
// forceWrite bypasses the hash guard's "already current, nothing to do" skip.
// Pass it for writes the USER explicitly asked for (the popup making an item
// current), because the guard can be stale: any clipboard change we couldn't
// read leaves it asserting the previous content is still there. Incoming
// network payloads must NOT set it — there the guard is real echo suppression.
void SetClipboardData(
    std::shared_ptr<const ClipboardPayload> payload,
    bool markAsClippOriginated = true,
    bool forceWrite = false);
