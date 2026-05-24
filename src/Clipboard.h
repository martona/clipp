#pragma once
#include "platform.h"
#include "ClipboardData.h"

// Creates an invisible window for clipboard notifications
using ClipboardCallback = void(*)(PlatformWindowHandle);

// Creates an invisible window for clipboard notifications, with callback
PlatformWindowHandle CreateClipboardNotificationWindow(ClipboardCallback cb);

// Starts the clipboard notification thread. Returns true on success.
bool StartClipboardNotification(ClipboardCallback callback);

// Stops the clipboard notification thread. Blocks until the thread exits.
void StopClipboardNotification();

// Reads the clipboard data and returns it as packet
ClipboardPayload ReadClipboardData(PlatformWindowHandle hwnd);
bool IsClipboardDataCurrent(const ClipboardPayload& payload);
void SetClipboardData(ClipboardPayload& payload, bool markAsClippOriginated = true);
