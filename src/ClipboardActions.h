#pragma once

#include "ClipboardPayload.h"

#include <cstdint>
#include <memory>

// UI-invoked clipboard actions shared by the settings page and the popup, plus
// the default-register mirror every clipboard-changing path feeds. Declared
// unconditionally; defined only on desktop daemon builds (ClipboardActions.cpp
// gates on CLIPP_REGISTERS_DAEMON — the same population that runs the register
// store these helpers touch).
namespace clipp {

// Reflect a text clipboard payload into the default ("") register so `ls` can
// show the live clipboard. Observational, local-only, text-only — a no-op for
// images or undecodable payloads. Also advances the persisted HLC floor
// (batched).
void MirrorClipboardToDefaultRegister(const std::shared_ptr<const ClipboardPayload>& payload);

// MRU re-share ("make this item current again"): clone the stored payload with
// the SAME eventGuid/origin and a strictly-bumped meta.timestamp, write the OS
// clipboard (hash guard armed — the watcher sees an echo, not a fresh event),
// relocate the item locally, mirror the default register, and broadcast the
// clone. Old peers set their clipboard and dedup the store op; new peers
// relocate too. False for unknown items and private placeholders.
bool ReshareActivityItem(uint64_t itemID);

// Best-effort mesh delete: remove the item locally and broadcast a CDEL frame
// (16-byte eventGuid) to every connected peer — old builds log-and-ignore the
// unknown tag. Unstamped (all-zero guid) items delete locally only. Returns
// the local removal result.
bool DeleteActivityItemEverywhere(uint64_t itemID);

}
