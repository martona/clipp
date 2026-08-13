#pragma once

// platform.h owns the Windows include order (WIN32_LEAN_AND_MEAN + WinSock2
// before windows.h); never pull <Windows.h> raw ahead of it.
#include "platform.h"

// The visual-paste popup: a borderless, topmost, NEVER-ACTIVATING XAML-island
// window summoned by the global hotkey (or the tray menu) — the Win+V model.
// The user's window keeps the system foreground, focus, and caret for the
// popup's entire life; keyboard input reaches the popup through a low-level
// keyboard hook that eats each key and re-posts it into the island's input
// pipeline (armed only while visible; see the input-routing section in the
// .cpp). Shows registers + the activity stream, filters as you type,
// Enter/double-click makes the selected item current everywhere (MRU
// re-share) and then PASTES it — a synthetic Ctrl+V into a window that never
// lost focus, so no restoration or activation polling is involved; Shift
// suppresses the keystroke. Del deletes everywhere, Esc closes. Light dismiss
// = a click on any foreign window (LL mouse hook) or a foreground change
// (winevent watcher).
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
