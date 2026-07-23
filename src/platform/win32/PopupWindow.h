#pragma once

// platform.h owns the Windows include order (WIN32_LEAN_AND_MEAN + WinSock2
// before windows.h); never pull <Windows.h> raw ahead of it.
#include "platform.h"

// The visual-paste popup: a borderless, topmost XAML-island window summoned by
// the global hotkey (or the tray menu). Shows the activity stream (registers
// group arrives with the promote flow), filters as you type, Enter/double-click
// makes the selected item current everywhere (MRU re-share), Del deletes it
// everywhere, Esc closes. Focus contract: the previously-focused window is
// captured at summon and restored on dismissal, so the user's manual Ctrl+V
// lands where they were — the later synthetic-paste feature builds on exactly
// this flow. Light-dismiss on focus loss deliberately does NOT restore focus
// (the user just gave it to someone else).
//
// All entry points are UI-thread (the tray message loop's thread). The window
// is created lazily on first summon and kept alive after (the XAML island's
// cold start is paid once).
namespace clipp {

// Hotkey / tray-menu entry point: show centered on the cursor's monitor, or
// hide if currently visible.
void TogglePopupWindow();

// Forward keyboard messages to the island while the popup is visible. Called
// from the tray message loop, next to the main dialog's PreTranslateMessage.
bool PopupPreTranslateMessage(MSG* msg);

// Tray shutdown: tear the window down.
void DestroyPopupWindow();

}
