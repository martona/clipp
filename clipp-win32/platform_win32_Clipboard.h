#pragma once
#include <windows.h>
#include "ClipboardData.h"

// Creates an invisible window for clipboard notifications
using ClipboardCallback = void(*)(HWND);

// Creates an invisible window for clipboard notifications, with callback
HWND CreateClipboardNotificationWindow(ClipboardCallback cb);

// Starts the clipboard notification thread. Returns true on success.
bool StartClipboardNotification(ClipboardCallback callback);

// Stops the clipboard notification thread. Blocks until the thread exits.
void StopClipboardNotification();

// Reads the clipboard data and returns it as packet
ClipboardPayload ReadClipboardData(HWND hwnd);
void SetClipboardData(const ClipboardPayload& payload);

using ClipboardCallback = void(*)(HWND);

