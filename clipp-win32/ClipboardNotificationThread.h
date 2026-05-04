#pragma once
#include <windows.h>
#include "ClipboardData.h"

using ClipboardCallback = void(*)(HWND);

// Starts the clipboard notification thread. Returns true on success.
bool StartClipboardNotification(ClipboardCallback callback);

// Stops the clipboard notification thread. Blocks until the thread exits.
void StopClipboardNotification();

// Reads the clipboard data and returns it as packet
ClipboardPayload ReadClipboardData(HWND hwnd);
