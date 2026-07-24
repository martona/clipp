#pragma once

#include "ClipboardPayload.h"

#include <cstdint>
#include <memory>
#include <string>

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

// ---- Named-register actions (the popup's registers column) ----
// Each performs the local store op AND the mesh half the register gateway does
// for one-shot clients (Peer.cpp REGW-relay): note the HLC floor, then push the
// resulting record — value or tombstone — as a REGW to register-serving peers.
// Names arrive already normalized/validated (the GUI editors run the shared
// validator; the store re-checks anyway).

// Enter on a register row: make its content the live clipboard everywhere.
// The in-process equivalent of `clipp put <name>` — Read (touches), build the
// same item `clipp copy` would send (canonical text + NUL, or the raw stream
// with the BinaryHeader's format), stamp a FRESH local origin (new eventGuid:
// this is a new clipboard event, not a re-share), then the same apply tail as
// a re-share: OS clipboard, activity store, default-register mirror, mesh
// broadcast. A PRIVATE register carries SOURCE_MARKED_PRIVATE onto the event.
bool MakeRegisterCurrent(const std::string& name);

// "Save" in the popup: promote an activity item into a named register. Text
// stores its logical content (capture-convention NUL stripped, same as the ""
// mirror); images store an EncodeBinaryValue-wrapped stream with the
// BinaryHeader flag. False for unsupported formats, empty payloads, or a
// refused store write. The caller picks the name (NextAutoRegisterName) and
// decides markPrivate (source-marked or heuristic-masked items carry it).
bool SaveActivityItemAsRegister(uint64_t itemID, const std::string& name, bool markPrivate);

// Del on a register row: tombstone locally and broadcast the tombstone. False
// when there was no live register to delete.
bool DeleteRegisterEverywhere(const std::string& name);

// Toolbar privacy toggle: rewrite the register in place with PRIVATE set or
// cleared. Same name, same value — an LWW overwrite of the one record slot,
// no tombstone involved. Broadcasts the result.
bool SetRegisterPrivate(const std::string& name, bool isPrivate);

// Inline-rename commit: upsert the record under the new name, then tombstone
// the old one — both broadcast, in that order, so a crash in between leaves
// the register duplicated rather than lost. Same-name is a successful no-op;
// an invalid target name or a vanished source fails.
bool RenameRegister(const std::string& oldName, const std::string& newName);

}
