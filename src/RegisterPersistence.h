#pragma once

#include "RegisterStore.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Sealed-snapshot persistence for the named-register store. The popup's "Save"
// verb created a durability expectation that persistence-by-replication alone
// can't honor (single-device users; whole-fleet restarts), so the full sync
// set — values AND tombstones; the "" clipboard mirror excluded — is written
// to disk as ONE encrypted blob. The clipboard stream stays ephemeral.
//
// This file is the PURE half: byte packing, AEAD sealing, and file I/O — no
// globals, no threads, no logging (doctest covers it; iOS reuses it whole).
// The daemon's wiring (key/path resolution, dirty debounce, startup/shutdown)
// lives in RegisterPersistenceRuntime.
//
// Blob layout:  magic "CLPR" | u8 version | 24-byte XChaCha20 nonce | AEAD ct
// Plaintext:    u32 count | count x (u32 len | RegisterWire::EncodeRecord bytes)
// All integers big-endian. The magic+version prefix doubles as the AEAD's
// associated data, so header tampering breaks authentication instead of
// reaching a future decoder. Callers must have run sodium_init().
namespace RegisterPersistence {

inline constexpr size_t kSealKeyBytes = 32;
using SealKey = std::array<unsigned char, kSealKeyBytes>;

// records -> sealed blob. Records whose wire encoding fails (over-cap value)
// are skipped — a snapshot never aborts wholesale over one record.
std::vector<unsigned char> SealSnapshot(const std::vector<RegisterRecord>& records,
                                        const SealKey& key);

// sealed blob -> records. False on wrong magic/version, failed authentication
// (wrong key or tampering), or a malformed frame; outRecords is untouched
// then. Once authentication passes, interior records that fail to decode are
// skipped rather than failing the whole snapshot (the mesh heals data; losing
// everything over one record heals nothing). Empty-name records (the mirror
// is never sealed; a forged one must not appear) are skipped likewise.
bool TryOpenSnapshot(const std::vector<unsigned char>& blob, const SealKey& key,
                     std::vector<RegisterRecord>& outRecords);

// Atomic save: sibling temp file + flush-to-disk + rename over. UTF-8 path.
bool SaveSnapshotFile(const std::string& utf8Path,
                      const std::vector<RegisterRecord>& records,
                      const SealKey& key);

enum class LoadResult {
    Loaded,   // outRecords filled (possibly with zero records)
    NoFile,   // nothing on disk — first run for this key
    Corrupt,  // unreadable or unauthentic; the carcass was renamed aside
};

// Defensive load: any failure renames the file aside (".corrupt", replacing a
// previous carcass) and reports Corrupt so the caller starts empty — never a
// boot loop.
LoadResult LoadSnapshotFile(const std::string& utf8Path, const SealKey& key,
                            std::vector<RegisterRecord>& outRecords);

}  // namespace RegisterPersistence
