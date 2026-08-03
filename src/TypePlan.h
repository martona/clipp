#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Text-typing engine, platform-neutral half. Turns text into a keystroke
// schedule that a platform backend replays into the OS input queue, for
// windows that cannot take a paste at all: iKVM/BMC consoles, virt-manager /
// SPICE / VNC viewers, RDP sessions with clipboard redirection off.
//
// Design borrowed from ../hitsc (which drives USB HID over a BMC tunnel and
// has already found the edge cases):
//   * SCANCODE-shaped output, not unicode injection. The viewers this targets
//     translate hardware key events into their wire protocol; a synthesized
//     unicode character (VK_PACKET, scancode 0) is dropped by most of them.
//   * ALL-OR-NOTHING translation. If any character can't be produced by the
//     active layout, nothing is typed and the caller reports exactly which
//     character failed. A partial "best effort" would silently punch holes in
//     the middle of a config file being pasted into a console.
//   * Consequence, accepted: scancodes are interpreted by the RECEIVER's
//     layout. A host/guest layout mismatch garbles text exactly as it would
//     on a physical KVM.

namespace clipp {

// A platform key code: Win32 virtual-key (VK_*) here; a macOS backend would
// carry CGKeyCode in the same slot.
using TypeKeyCode = uint16_t;

// One keystroke: the modifiers to hold, plus the character key.
struct KeyChord {
    std::vector<TypeKeyCode> modifiers;
    TypeKeyCode key = 0;
};

// The first character the active layout can't produce. line/column are 1-based
// and counted in Unicode scalars; column resets after each newline.
struct TypeError {
    char32_t codepoint = 0;
    int line = 1;
    int column = 1;
};

struct TypePlan {
    std::vector<KeyChord> chords;
    int characterCount = 0;  // typable characters consumed (incl. Enter/Tab)
    int enterCount = 0;      // Enter presses — each one may run a command
};

// Result of a layout translation: a plan, or the first untypable character.
struct TypeResult {
    bool ok = false;
    TypePlan plan;
    TypeError error;
};

// One event as it goes to the OS.
struct TypeKeyEvent {
    TypeKeyCode key = 0;
    bool down = false;
};

// A plan expanded into the exact event stream, plus the counts the UI quotes.
struct TypeSchedule {
    std::vector<TypeKeyEvent> events;
    int characterCount = 0;
    int enterCount = 0;
};

// Milliseconds per event. Every state change must outlive one poll of whatever
// input path is downstream (BMC keyboard emulation, a SPICE/VNC input queue),
// so this is deliberately slow: two events per plain character puts typing at
// roughly 30 characters/second. The single tuning knob if a target turns out
// to drop keys.
inline constexpr int kTypeEventIntervalMs = 15;

// Expand a plan into down/up events, HOLDING shared modifiers across
// consecutive chords rather than releasing and re-pressing them (a run of
// capitals presses Shift once). This is the only place event counts come
// from, so the ETA the user is shown matches what actually runs.
TypeSchedule BuildTypeSchedule(const TypePlan& plan);

// Whole-run duration at kTypeEventIntervalMs, rounded to seconds.
int EstimateTypeSeconds(const TypeSchedule& schedule);

}  // namespace clipp
