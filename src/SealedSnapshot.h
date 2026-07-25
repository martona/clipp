#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The generic sealed-records-file layer shared by every at-rest snapshot kind
// (registers, the iOS clipboard blob, the share-extension inbox drops). Owns the
// byte framing, AEAD sealing, and defensive file I/O; consumers own their record
// codecs (RegisterWire::EncodeRecord, ClipboardPersistence::EncodeRecord) and
// the semantic skip-bad-records-on-load policy. Extracted from the original
// RegisterPersistence so the clipboard side generalizes instead of forking.
//
// Blob layout:  magic[4] | u8 version | 24-byte XChaCha20 nonce | AEAD ct
// Plaintext:    u32 count | count x (u32 len | record bytes)
// All integers big-endian. The magic+version prefix doubles as the AEAD's
// associated data, so header tampering breaks authentication instead of
// reaching a future decoder — and a blob sealed under one magic/key-role can
// never open as another kind. Callers must have run sodium_init().
namespace SealedSnapshot {

inline constexpr size_t kSealKeyBytes = 32;
using SealKey = std::array<unsigned char, kSealKeyBytes>;
using Magic = std::array<unsigned char, 4>;

// Consumer caps are far lower (registers: 1024); this only stops a forged count
// from driving the (authenticated, so practically unreachable) decode loop to
// absurdity.
inline constexpr uint32_t kMaxRecords = 65535;

// records -> sealed blob. Empty record vectors are skipped — the convention for
// "this record failed to encode"; a snapshot never aborts wholesale over one.
// The plaintext scratch is zeroized before returning (values may be sensitive).
std::vector<unsigned char> Seal(const Magic& magic, uint8_t version,
                                const std::vector<std::vector<unsigned char>>& records,
                                const SealKey& key);

// sealed blob -> raw record byte vectors. False on wrong magic/version, failed
// authentication (wrong key or tampering), or a malformed count/length frame
// (including trailing garbage); outRecords is untouched then. Interior record
// DECODING is the consumer's job — and so is skipping records that fail it.
bool TryOpen(const Magic& magic, uint8_t version,
             const std::vector<unsigned char>& blob, const SealKey& key,
             std::vector<std::vector<unsigned char>>& outRecords);

// Atomic save: sibling temp file + flush-to-disk + rename over. UTF-8 path.
bool SaveFile(const std::string& utf8Path, const Magic& magic, uint8_t version,
              const std::vector<std::vector<unsigned char>>& records,
              const SealKey& key);

enum class LoadResult {
    Loaded,   // outRecords filled (possibly with zero records)
    NoFile,   // nothing on disk — first run for this key
    Corrupt,  // unreadable or unauthentic; the carcass was renamed aside
};

// Defensive load: any failure renames the file aside (".corrupt", replacing a
// previous carcass) and reports Corrupt so the caller starts empty — never a
// boot loop.
LoadResult LoadFile(const std::string& utf8Path, const Magic& magic, uint8_t version,
                    const SealKey& key,
                    std::vector<std::vector<unsigned char>>& outRecords);

}  // namespace SealedSnapshot
