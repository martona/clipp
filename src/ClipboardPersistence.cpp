#include "ClipboardPersistence.h"

#include "ClipboardLimits.h"
#include "NetworkDefs.h"

#include <cstring>

namespace ClipboardPersistence {

namespace {

constexpr SealedSnapshot::Magic kSnapshotMagic = { 'C', 'L', 'P', 'C' };
constexpr SealedSnapshot::Magic kInboxMagic = { 'C', 'L', 'P', 'I' };
constexpr uint8_t kVersion = 1;

// Matches ClipboardWire's frame budget: a record that couldn't ride the wire
// has no business in a snapshot either.
constexpr size_t kMaxBodyBytes = 64u * 1024u * 1024u;

}  // namespace

std::vector<unsigned char> EncodeRecord(const ClipboardPayload& payload) {
    const std::vector<unsigned char>& body = payload.EncodedBytes();
    if (body.size() > kMaxBodyBytes) {
        return {};
    }

    NetworkDefs::ClipboardMessage netMsg = payload.meta;
    NetworkDefs::HostToNetworkClipboardMessage(netMsg);

    std::vector<unsigned char> record;
    record.reserve(4 + sizeof(netMsg) + body.size());
    record.push_back('C');
    record.push_back('L');
    record.push_back('I');
    record.push_back('P');
    const auto* meta = reinterpret_cast<const unsigned char*>(&netMsg);
    record.insert(record.end(), meta, meta + sizeof(netMsg));
    record.insert(record.end(), body.begin(), body.end());
    return record;
}

bool TryDecodeRecord(const std::vector<unsigned char>& record, ClipboardPayload& out) {
    constexpr size_t kClipHeaderSize = sizeof(NetworkDefs::ClipboardMessage);
    if (record.size() < 4 + kClipHeaderSize) {
        return false;
    }
    if (std::memcmp(record.data(), "CLIP", 4) != 0) {
        return false;
    }

    out = ClipboardPayload{};
    std::memcpy(&out.meta, record.data() + 4, kClipHeaderSize);
    NetworkDefs::NetworkToHostClipboardMessage(out.meta);

    const size_t expectedBodyBytes = record.size() - 4 - kClipHeaderSize;
    if (out.meta.payloadDataSize != static_cast<uint64_t>(expectedBodyBytes)) {
        return false;
    }
    if (out.meta.uncompressedDataSize > ClipboardLimits::kMaxDecompressedClipboardBytes) {
        return false;
    }
    if (out.meta.isCompressed == 0
        && out.meta.payloadDataSize != out.meta.uncompressedDataSize) {
        return false;
    }

    std::vector<unsigned char> body;
    if (expectedBodyBytes > 0) {
        body.assign(record.data() + 4 + kClipHeaderSize,
                    record.data() + 4 + kClipHeaderSize + expectedBodyBytes);
    }
    out.SetEncodedBytes(std::move(body));
    return true;
}

bool SaveSnapshotFile(const std::string& utf8Path,
                      const std::vector<std::shared_ptr<const ClipboardPayload>>& payloads,
                      const SealKey& key) {
    std::vector<std::vector<unsigned char>> records;
    records.reserve(payloads.size());
    for (const auto& payload : payloads) {
        if (payload) {
            records.push_back(EncodeRecord(*payload));
        }
    }
    return SealedSnapshot::SaveFile(utf8Path, kSnapshotMagic, kVersion, records, key);
}

LoadResult LoadSnapshotFile(const std::string& utf8Path, const SealKey& key,
                            std::vector<ClipboardPayload>& outPayloads) {
    std::vector<std::vector<unsigned char>> raw;
    const LoadResult result =
        SealedSnapshot::LoadFile(utf8Path, kSnapshotMagic, kVersion, key, raw);
    if (result != LoadResult::Loaded) {
        return result;
    }
    std::vector<ClipboardPayload> payloads;
    payloads.reserve(raw.size());
    for (const auto& record : raw) {
        ClipboardPayload payload;
        if (TryDecodeRecord(record, payload)) {
            payloads.push_back(std::move(payload));  // undecodable records: skip
        }
    }
    outPayloads = std::move(payloads);
    return LoadResult::Loaded;
}

bool SaveInboxItemFile(const std::string& utf8Path, const ClipboardPayload& payload,
                       const SealKey& key) {
    const std::vector<unsigned char> record = EncodeRecord(payload);
    if (record.empty()) {
        return false;  // an inbox drop is one item; nothing to skip TO
    }
    return SealedSnapshot::SaveFile(utf8Path, kInboxMagic, kVersion, { record }, key);
}

LoadResult LoadInboxItemFile(const std::string& utf8Path, const SealKey& key,
                             ClipboardPayload& outPayload) {
    std::vector<std::vector<unsigned char>> raw;
    const LoadResult result =
        SealedSnapshot::LoadFile(utf8Path, kInboxMagic, kVersion, key, raw);
    if (result != LoadResult::Loaded) {
        return result;
    }
    ClipboardPayload payload;
    if (raw.size() != 1 || !TryDecodeRecord(raw[0], payload)) {
        return LoadResult::Corrupt;  // authenticated but not a single-item drop
    }
    outPayload = std::move(payload);
    return LoadResult::Loaded;
}

}  // namespace ClipboardPersistence
