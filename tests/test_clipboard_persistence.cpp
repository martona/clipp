#include <doctest/doctest.h>

#include "ClipboardFormat.h"
#include "ClipboardPersistence.h"
#include "NetworkDefs.h"
#include "RegisterPersistence.h"
#include "RegisterStore.h"
#include "SealedSnapshot.h"

#include <sodium.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void RequireSodium() {
    REQUIRE(sodium_init() >= 0);
}

ClipboardPersistence::SealKey MakeKey(unsigned char fill) {
    ClipboardPersistence::SealKey key{};
    key.fill(fill);
    return key;
}

ClipboardPayload MakeTextPayload(const std::string& text, uint64_t timestamp, uint32_t flags = 0) {
    ClipboardPayload payload{};
    payload.meta.formatId = CLIPP_FORMAT_UTF8;
    payload.meta.flags = flags;
    payload.meta.timestamp = timestamp;
    payload.meta.eventGuid[0] = static_cast<uint8_t>(timestamp & 0xFF);
    payload.meta.eventGuid[15] = 0x5A;
    std::vector<unsigned char> bytes(text.begin(), text.end());
    bytes.push_back('\0');
    REQUIRE(payload.SetUncompressedBytes(std::move(bytes)));
    return payload;
}

void CheckPayloadsEqual(const ClipboardPayload& a, const ClipboardPayload& b) {
    CHECK(a.meta.formatId == b.meta.formatId);
    CHECK(a.meta.flags == b.meta.flags);
    CHECK(a.meta.timestamp == b.meta.timestamp);
    CHECK(a.meta.payloadDataSize == b.meta.payloadDataSize);
    CHECK(a.meta.uncompressedDataSize == b.meta.uncompressedDataSize);
    CHECK(a.meta.isCompressed == b.meta.isCompressed);
    CHECK(std::memcmp(a.meta.eventGuid, b.meta.eventGuid, sizeof(a.meta.eventGuid)) == 0);
    CHECK(a.EncodedBytes() == b.EncodedBytes());
}

std::filesystem::path TempFilePath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

struct TempFileGuard {
    std::filesystem::path path;
    ~TempFileGuard() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::path corrupt = path;
        corrupt += ".corrupt";
        std::filesystem::remove(corrupt, ignored);
    }
};

}  // namespace

TEST_CASE("clipboard record round-trips through the CLIP-frame codec") {
    RequireSodium();
    const ClipboardPayload payload =
        MakeTextPayload("hello persistence", 1'700'000'000'123ull,
                        NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE);

    const auto record = ClipboardPersistence::EncodeRecord(payload);
    REQUIRE(!record.empty());

    ClipboardPayload decoded;
    REQUIRE(ClipboardPersistence::TryDecodeRecord(record, decoded));
    CheckPayloadsEqual(payload, decoded);

    // Sanity on the layout contract: it IS the wire CLIP frame.
    CHECK(record[0] == 'C');
    CHECK(record[3] == 'P');
}

TEST_CASE("clipboard snapshot round-trips text, binary and empty-body records") {
    RequireSodium();
    const auto key = MakeKey(0x11);
    TempFileGuard guard{ TempFilePath("clipp-test-clipboard.bin") };

    auto text = std::make_shared<const ClipboardPayload>(
        MakeTextPayload("first", 1'700'000'000'001ull));

    ClipboardPayload imageSrc{};
    imageSrc.meta.formatId = CLIPP_FORMAT_PNG;
    imageSrc.meta.timestamp = 1'700'000'000'002ull;
    imageSrc.meta.eventGuid[1] = 0x22;
    std::vector<unsigned char> pngish{ 0x89, 'P', 'N', 'G', 0x00, 0x01, 0x02 };
    REQUIRE(imageSrc.SetUncompressedBytes(std::move(pngish)));
    auto image = std::make_shared<const ClipboardPayload>(std::move(imageSrc));

    // A source-marked-private placeholder: flags + guid, no content at all.
    ClipboardPayload placeholderSrc{};
    placeholderSrc.meta.formatId = CLIPP_FORMAT_UTF8;
    placeholderSrc.meta.flags = NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE;
    placeholderSrc.meta.timestamp = 1'700'000'000'003ull;
    placeholderSrc.meta.eventGuid[2] = 0x33;
    auto placeholder = std::make_shared<const ClipboardPayload>(std::move(placeholderSrc));

    REQUIRE(ClipboardPersistence::SaveSnapshotFile(
        guard.path.string(), { text, image, placeholder }, key));

    std::vector<ClipboardPayload> loaded;
    REQUIRE(ClipboardPersistence::LoadSnapshotFile(guard.path.string(), key, loaded)
            == ClipboardPersistence::LoadResult::Loaded);
    REQUIRE(loaded.size() == 3);
    CheckPayloadsEqual(*text, loaded[0]);
    CheckPayloadsEqual(*image, loaded[1]);
    CheckPayloadsEqual(*placeholder, loaded[2]);
}

TEST_CASE("clipboard snapshot refuses the wrong key and quarantines the file") {
    RequireSodium();
    TempFileGuard guard{ TempFilePath("clipp-test-clipboard-badkey.bin") };
    auto item = std::make_shared<const ClipboardPayload>(
        MakeTextPayload("secret", 1'700'000'000'004ull));
    REQUIRE(ClipboardPersistence::SaveSnapshotFile(guard.path.string(), { item }, MakeKey(0x11)));

    std::vector<ClipboardPayload> loaded;
    CHECK(ClipboardPersistence::LoadSnapshotFile(guard.path.string(), MakeKey(0x22), loaded)
          == ClipboardPersistence::LoadResult::Corrupt);
    CHECK(loaded.empty());

    std::filesystem::path corrupt = guard.path;
    corrupt += ".corrupt";
    CHECK(std::filesystem::exists(corrupt));
    CHECK(!std::filesystem::exists(guard.path));
    // A rerun finds nothing on disk — never a boot loop.
    CHECK(ClipboardPersistence::LoadSnapshotFile(guard.path.string(), MakeKey(0x11), loaded)
          == ClipboardPersistence::LoadResult::NoFile);
}

TEST_CASE("register and clipboard blobs can never cross-decode") {
    RequireSodium();
    // Same key on purpose: even a role-derivation bug upstream must not let one
    // kind open as the other — the magic-as-AD does the separating here.
    const auto key = MakeKey(0x33);
    TempFileGuard guard{ TempFilePath("clipp-test-cross.bin") };

    RegisterRecord record;
    record.name = "reg";
    record.value = "value";
    record.written = Hlc{ 1'700'000'000'005ull, 0 };
    record.touched = record.written;
    REQUIRE(RegisterPersistence::SaveSnapshotFile(guard.path.string(), { record }, key));

    std::vector<ClipboardPayload> loaded;
    CHECK(ClipboardPersistence::LoadSnapshotFile(guard.path.string(), key, loaded)
          == ClipboardPersistence::LoadResult::Corrupt);
}

TEST_CASE("inbox item round-trips; multi-record inbox blobs are rejected") {
    RequireSodium();
    const auto key = MakeKey(0x44);
    TempFileGuard one{ TempFilePath("clipp-test-inbox-one.bin") };
    TempFileGuard two{ TempFilePath("clipp-test-inbox-two.bin") };

    const ClipboardPayload item = MakeTextPayload("deferred share", 1'700'000'000'006ull);
    REQUIRE(ClipboardPersistence::SaveInboxItemFile(one.path.string(), item, key));

    ClipboardPayload loaded;
    REQUIRE(ClipboardPersistence::LoadInboxItemFile(one.path.string(), key, loaded)
            == ClipboardPersistence::LoadResult::Loaded);
    CheckPayloadsEqual(item, loaded);

    // Hand-build a two-record blob under the inbox magic: authenticated but not
    // a single-item drop -> Corrupt.
    const auto record = ClipboardPersistence::EncodeRecord(item);
    constexpr SealedSnapshot::Magic kInboxMagic = { 'C', 'L', 'P', 'I' };
    REQUIRE(SealedSnapshot::SaveFile(two.path.string(), kInboxMagic, 1,
                                     { record, record }, key));
    CHECK(ClipboardPersistence::LoadInboxItemFile(two.path.string(), key, loaded)
          == ClipboardPersistence::LoadResult::Corrupt);
}

TEST_CASE("tampering with a sealed clipboard blob breaks authentication") {
    RequireSodium();
    const auto key = MakeKey(0x55);
    auto item = std::make_shared<const ClipboardPayload>(
        MakeTextPayload("tamper me", 1'700'000'000'007ull));
    std::vector<std::vector<unsigned char>> records{ ClipboardPersistence::EncodeRecord(*item) };
    constexpr SealedSnapshot::Magic kMagic = { 'C', 'L', 'P', 'C' };
    auto blob = SealedSnapshot::Seal(kMagic, 1, records, key);
    REQUIRE(!blob.empty());

    blob[blob.size() / 2] ^= 0x01;
    std::vector<std::vector<unsigned char>> opened;
    CHECK(!SealedSnapshot::TryOpen(kMagic, 1, blob, key, opened));
}
