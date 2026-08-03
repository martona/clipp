#pragma once

#include "TypePlan.h"

namespace clipp {

// Replays a keystroke schedule into the OS input queue with SendInput, paced
// one event per kTypeEventIntervalMs tick, with a sticky progress toast near
// the tray and abort on ANY physical keystroke or mouse click.
//
// Deliberately target-blind: whatever holds the keyboard when the run starts
// receives the text. The caller is responsible for having restored the right
// foreground window first (the popup does this with its existing paste-target
// polling), and the user for pointing it somewhere sensible — a keyboard
// away or a click stops it instantly.
//
// UI-thread only (the tray message loop): the pacing timer and the low-level
// input hooks both need that thread's message pump.

// Starts a run. False if one is already in flight, or the schedule is empty.
bool StartTyping(TypeSchedule schedule);

bool IsTyping();

// Stops early: releases every key still held down, then tears the run down.
void CancelTyping();

// Tray shutdown.
void ShutdownTyping();

}  // namespace clipp
