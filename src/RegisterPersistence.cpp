#include "RegisterPersistence.h"

#include "RegisterWire.h"
#include "SealedSnapshot.h"

namespace RegisterPersistence {

namespace {

// "CLPR" v1 — unchanged from the original single-purpose implementation, so
// snapshots written before the SealedSnapshot extraction open bit-identically.
constexpr SealedSnapshot::Magic kMagic = { 'C', 'L', 'P', 'R' };
constexpr uint8_t kVersion = 1;

static_assert(kSealKeyBytes == SealedSnapshot::kSealKeyBytes);

std::vector<std::vector<unsigned char>> EncodeRecords(const std::vector<RegisterRecord>& records) {
    std::vector<std::vector<unsigned char>> encoded;
    encoded.reserve(records.size());
    for (const auto& record : records) {
        // Over-cap or otherwise unencodable records come back empty; the seal
        // layer skips those — a snapshot never aborts wholesale over one.
        encoded.push_back(RegisterWire::EncodeRecord(record, 0));
    }
    return encoded;
}

std::vector<RegisterRecord> DecodeRecords(const std::vector<std::vector<unsigned char>>& raw) {
    std::vector<RegisterRecord> records;
    records.reserve(raw.size());
    for (const auto& body : raw) {
        RegisterRecord record;
        uint8_t transportFlags = 0;
        if (!RegisterWire::TryDecodeRecord(body, record, transportFlags)
            || record.name.empty()) {
            continue;  // authenticated but undecodable/mirror-named: skip it
        }
        records.push_back(std::move(record));
    }
    return records;
}

}  // namespace

std::vector<unsigned char> SealSnapshot(const std::vector<RegisterRecord>& records,
                                        const SealKey& key) {
    return SealedSnapshot::Seal(kMagic, kVersion, EncodeRecords(records), key);
}

bool TryOpenSnapshot(const std::vector<unsigned char>& blob, const SealKey& key,
                     std::vector<RegisterRecord>& outRecords) {
    std::vector<std::vector<unsigned char>> raw;
    if (!SealedSnapshot::TryOpen(kMagic, kVersion, blob, key, raw)) {
        return false;
    }
    outRecords = DecodeRecords(raw);
    return true;
}

bool SaveSnapshotFile(const std::string& utf8Path,
                      const std::vector<RegisterRecord>& records,
                      const SealKey& key) {
    return SealedSnapshot::SaveFile(utf8Path, kMagic, kVersion, EncodeRecords(records), key);
}

LoadResult LoadSnapshotFile(const std::string& utf8Path, const SealKey& key,
                            std::vector<RegisterRecord>& outRecords) {
    std::vector<std::vector<unsigned char>> raw;
    switch (SealedSnapshot::LoadFile(utf8Path, kMagic, kVersion, key, raw)) {
    case SealedSnapshot::LoadResult::NoFile:
        return LoadResult::NoFile;
    case SealedSnapshot::LoadResult::Corrupt:
        return LoadResult::Corrupt;
    case SealedSnapshot::LoadResult::Loaded:
        break;
    }
    outRecords = DecodeRecords(raw);
    return LoadResult::Loaded;
}

}  // namespace RegisterPersistence
