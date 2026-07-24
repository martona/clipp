#pragma once

#ifdef __APPLE__

#import <Foundation/Foundation.h>

#include <cstdint>

// The visual-paste popup, macOS shell: a NONACTIVATING borderless NSPanel
// summoned by a global Carbon hotkey or the status menu. Because the panel
// becomes key without activating the app, the previously frontmost app never
// deactivates — there is no focus to capture or restore; dismissing simply
// returns the keyboard to whoever had it. Mirrors the win32 popup's behavior:
// PopupModel drives two columns (Registers | Clipboard), the filter field owns
// the keyboard (launcher pattern), Enter/double-click makes the selection
// current mesh-wide and then PASTES it (⌘V via CGEventPost — gated on the
// Accessibility grant, silently skipped without it; Shift suppresses the
// keystroke), and the toolbar fronts the shared ClipboardActions
// (save / paste / rename / privacy / delete / undo). While the Accessibility
// grant is missing, every summon shows an onboarding toast above the panel
// that walks the user to System Settings and polls until the grant lands.
//
// All entry points are main-thread. The panel is created lazily on first
// summon and kept alive after.
namespace clipp {

// Register the global hotkeys (Carbon RegisterEventHotKey — no accessibility
// permission needed, MAS-safe). Both chords come from Settings
// (popupHotkeyPrimary/Secondary, Carbon-modifier<<16|keycode encoding;
// defaults ⌘Insert + ⌃⌘V). Call once from the status-menu setup.
void InstallPopupHotkeys();

// Re-register both chords from Settings, replacing the current
// registrations (the settings page calls this after a change). Returns a
// bitmask of slots that FAILED to register (bit 0 = primary, bit 1 =
// secondary) — in practice "already taken by another app".
unsigned ReapplyPopupHotkeys();

// Human-readable chord for the settings page ("⌃⌘V", "Insert" spelled out);
// the NONE string for 0.
NSString* FormatPopupHotkeyChord(uint32_t chord);

// Menu-bar key-equivalent form of a chord, when it has one (function keys
// and Insert/Help do not). Returns NO — leave the menu item bare — otherwise.
BOOL PopupHotkeyMenuEquivalent(uint32_t chord, NSString** outKey, NSUInteger* outModifierFlags);

// Hotkey / menu entry point: show centered on the mouse's screen, or hide if
// currently visible.
void TogglePopupPanel();

// App shutdown: tear the panel and hotkeys down.
void DestroyPopupPanel();

}

#endif
