#pragma once

#include "ClipboardPayload.h"
#include "SealedSnapshot.h"

#include <memory>
#include <string>
#include <vector>

// At-rest persistence for clipboard/activity payloads — iOS only (the desktops
// keep the clipboard stream RAM-only by design; nothing links this there).
// Two blob kinds, both sealed by SealedSnapshot with their own KeyManager
// roles so neither can cross-decode with each other or with registers:
//   * the ACTIVITY SNAPSHOT ("CLPC", KeyRole::ClipboardStorage): the app's
//     history, one blob in the app container.
//   * INBOX ITEMS ("CLPI", KeyRole::ShareInbox): the share extension's
//     one-file-per-item deferred-share drops in the app group, ingested (and
//     deleted) by the main app.
//
// A record is byte-identical to the wire CLIP frame — 4-byte 'CLIP' tag,
// network-order NetworkDefs::ClipboardMessage, encoded body — so persisted
// items round-trip the exact same validation as mesh traffic and stay
// debuggable with the same eyes. Pure: no globals, no threads, no logging
// (decode failures are silent skips; the snapshot contract is defensive).
namespace ClipboardPersistence {

using SealKey = SealedSnapshot::SealKey;
using LoadResult = SealedSnapshot::LoadResult;

// payload -> CLIP-frame bytes. Empty on an over-cap body (the 64 MB wire frame
// limit) — the seal layer skips empties rather than aborting the snapshot.
std::vector<unsigned char> EncodeRecord(const ClipboardPayload& payload);

// CLIP-frame bytes -> payload. Mirrors ClipboardWire::TryDecodeClipboardFrame's
// checks (header size, body-size match, uncompressed cap, uncompressed-size
// invariant) without its logging. False leaves `out` unspecified.
bool TryDecodeRecord(const std::vector<unsigned char>& record, ClipboardPayload& out);

// --- the activity snapshot (one blob, newest-first, caller-bounded) ---

bool SaveSnapshotFile(const std::string& utf8Path,
                      const std::vector<std::shared_ptr<const ClipboardPayload>>& payloads,
                      const SealKey& key);

LoadResult LoadSnapshotFile(const std::string& utf8Path, const SealKey& key,
                            std::vector<ClipboardPayload>& outPayloads);

// --- inbox items (exactly one payload per file) ---

bool SaveInboxItemFile(const std::string& utf8Path, const ClipboardPayload& payload,
                       const SealKey& key);

// Loaded requires exactly one decodable record; a multi-record or empty blob
// reports Corrupt (and was quarantined by the seal layer).
LoadResult LoadInboxItemFile(const std::string& utf8Path, const SealKey& key,
                             ClipboardPayload& outPayload);

}  // namespace ClipboardPersistence
