#pragma once

#include <cstdint>
#include <string>

// The popup's global-summon hotkeys: two user-configurable chords
// (Settings::popupHotkeyPrimary/Secondary, RegisterHotKey encoding), both
// toggling the popup. Registration is tray-owned — the hidden tray window
// receives WM_HOTKEY — so the implementation lives in tray.cpp. All calls on
// the tray/UI thread.
namespace clipp {

// (Re-)register both chords from Settings, replacing any current
// registrations. Returns a bitmask of slots that FAILED to register (bit 0 =
// primary, bit 1 = secondary) — in practice "already taken by another app" —
// so the settings page can say so.
unsigned ReapplyPopupHotkeys();

// Human-readable chord ("Win + Ctrl + V"); the NONE string for 0.
std::wstring FormatPopupHotkeyChord(uint32_t chord);

}
