#pragma once
#include <windows.h>

// Creates an invisible window for clipboard notifications
using ClipboardCallback = void(*)(HWND);

// Creates an invisible window for clipboard notifications, with callback
HWND CreateClipboardNotificationWindow(ClipboardCallback cb);
