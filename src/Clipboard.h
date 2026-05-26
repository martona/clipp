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
void SetClipboardData(
    std::shared_ptr<const ClipboardPayload> payload,
    bool markAsClippOriginated = true);
