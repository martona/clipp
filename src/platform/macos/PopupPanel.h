#pragma once

#ifdef __APPLE__

// The visual-paste popup, macOS shell: a NONACTIVATING borderless NSPanel
// summoned by a global Carbon hotkey or the status menu. Because the panel
// becomes key without activating the app, the previously frontmost app never
// deactivates — there is no focus to capture or restore; dismissing simply
// returns the keyboard to whoever had it. Mirrors the win32 popup's behavior:
// PopupModel drives two columns (Registers | Clipboard), the filter field owns
// the keyboard (launcher pattern), Enter/double-click makes the selection
// current mesh-wide, and the toolbar fronts the shared ClipboardActions
// (save / copy / rename / privacy / delete / undo).
//
// All entry points are main-thread. The panel is created lazily on first
// summon and kept alive after.
namespace clipp {

// Register the global hotkeys (Carbon RegisterEventHotKey — no accessibility
// permission needed, MAS-safe): primary ⌘Insert (kVK_Help — the PC-Insert
// position; most Apple laptop keyboards lack the key) and secondary ⌃⌘V so
// laptops have a typable chord. Call once from the status-menu setup.
void InstallPopupHotkeys();

// Hotkey / menu entry point: show centered on the mouse's screen, or hide if
// currently visible.
void TogglePopupPanel();

// App shutdown: tear the panel and hotkeys down.
void DestroyPopupPanel();

}

#endif
