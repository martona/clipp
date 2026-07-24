#include <doctest/doctest.h>

#include "Hlc.h"
#include "HostId.h"
#include "RegisterPersistence.h"
#include "RegisterStore.h"

#include <sodium.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

HostId MakeHost(uint8_t tag) {
    HostId::Bytes b{};
    b[0] = tag;
    b[15] = 0xAB;
    return HostId(b);
}

RegisterRecord MakeRec(std::string name, std::string value, uint8_t flags) {
    RegisterRecord r;
    r.name = std::move(name);
    r.value = std::move(value);
    r.written = Hlc{ 1'700'000'000'000ull, 5 };
    r.touched = Hlc{ 1'700'000'000'123ull, 0 };
    r.originHostId = MakeHost(3);
    r.flags = flags;
    return r;
}

RegisterPersistence::SealKey MakeKey(unsigned char fill) {
    RegisterPersistence::SealKey key{};
    key.fill(fill);
    return key;
}

std::vector<RegisterRecord> SampleRecords() {
    return {
        MakeRec("url", "https://example.com", 0),
        MakeRec("pw", "s3cret", RegisterFlags::Private),
        MakeRec("gone", "", RegisterFlags::Tombstone),
        MakeRec("img", std::string("\x01\x00\x00\x08\x00\x00\x00\x07png....", 15),
                RegisterFlags::BinaryHeader),
        MakeRec("caf\xC3\xA9", "unicode name", 0),
    };
}

// Every test runs through here so sodium is initialized exactly once.
void RequireSodium() {
    REQUIRE(sodium_init() >= 0);
}

std::filesystem::path TempFile(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

void CleanupSnapshotFiles(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::path corrupt = path;
    corrupt += ".corrupt";
    std::filesystem::remove(corrupt, ignored);
    std::filesystem::path temp = path;
    temp += ".tmp";
    std::filesystem::remove(temp, ignored);
}

}  // namespace

TEST_CASE("sealed snapshot round-trips records, tombstones, and flags") {
    RequireSodium();
    const auto key = MakeKey(0x42);
    const auto records = SampleRecords();

    const auto blob = RegisterPersistence::SealSnapshot(records, key);
    REQUIRE(!blob.empty());

    std::vector<RegisterRecord> out;
    REQUIRE(RegisterPersistence::TryOpenSnapshot(blob, key, out));
    CHECK(out == records);
}

TEST_CASE("empty snapshot round-trips to zero records") {
    RequireSodium();
    const auto key = MakeKey(0x01);
    const auto blob = RegisterPersistence::SealSnapshot({}, key);
    REQUIRE(!blob.empty());
    std::vector<RegisterRecord> out{ MakeRec("stale", "left over", 0) };
    REQUIRE(RegisterPersistence::TryOpenSnapshot(blob, key, out));
    CHECK(out.empty());
}

TEST_CASE("wrong key fails authentication") {
    RequireSodium();
    const auto blob = RegisterPersistence::SealSnapshot(SampleRecords(), MakeKey(0x42));
    std::vector<RegisterRecord> out;
    CHECK_FALSE(RegisterPersistence::TryOpenSnapshot(blob, MakeKey(0x43), out));
    CHECK(out.empty());
}

TEST_CASE("tampering anywhere fails authentication") {
    RequireSodium();
    const auto key = MakeKey(0x42);
    const auto blob = RegisterPersistence::SealSnapshot(SampleRecords(), key);
    std::vector<RegisterRecord> out;

    SUBCASE("flipped ciphertext byte") {
        auto bad = blob;
        bad[bad.size() / 2] ^= 0x01;
        CHECK_FALSE(RegisterPersistence::TryOpenSnapshot(bad, key, out));
    }
    SUBCASE("flipped header byte (the AD)") {
        auto bad = blob;
        bad[0] ^= 0x01;  // magic breaks first...
        CHECK_FALSE(RegisterPersistence::TryOpenSnapshot(bad, key, out));
    }
    SUBCASE("bumped version byte") {
        auto bad = blob;
        bad[4] = 2;  // ...and even a plausible future version fails auth via the AD
        CHECK_FALSE(RegisterPersistence::TryOpenSnapshot(bad, key, out));
    }
    SUBCASE("truncated blob") {
        auto bad = blob;
        bad.resize(bad.size() - 1);
        CHECK_FALSE(RegisterPersistence::TryOpenSnapshot(bad, key, out));
    }
    SUBCASE("far too short") {
        std::vector<unsigned char> bad(blob.begin(), blob.begin() + 8);
        CHECK_FALSE(RegisterPersistence::TryOpenSnapshot(bad, key, out));
    }
}

TEST_CASE("snapshot file save/load round-trips atomically") {
    RequireSodium();
    const auto key = MakeKey(0x42);
    const auto path = TempFile("clipp-test-registers.bin");
    CleanupSnapshotFiles(path);
    const std::string utf8Path = path.string();
    const auto records = SampleRecords();

    REQUIRE(RegisterPersistence::SaveSnapshotFile(utf8Path, records, key));
    std::vector<RegisterRecord> out;
    CHECK(RegisterPersistence::LoadSnapshotFile(utf8Path, key, out)
          == RegisterPersistence::LoadResult::Loaded);
    CHECK(out == records);

    // Overwrite with a smaller set; the rename must replace, not append.
    const std::vector<RegisterRecord> fewer{ MakeRec("only", "one", 0) };
    REQUIRE(RegisterPersistence::SaveSnapshotFile(utf8Path, fewer, key));
    out.clear();
    CHECK(RegisterPersistence::LoadSnapshotFile(utf8Path, key, out)
          == RegisterPersistence::LoadResult::Loaded);
    CHECK(out == fewer);

    CleanupSnapshotFiles(path);
}

TEST_CASE("missing file reports NoFile") {
    RequireSodium();
    const auto path = TempFile("clipp-test-registers-absent.bin");
    CleanupSnapshotFiles(path);
    std::vector<RegisterRecord> out;
    CHECK(RegisterPersistence::LoadSnapshotFile(path.string(), MakeKey(0x42), out)
          == RegisterPersistence::LoadResult::NoFile);
}

TEST_CASE("corrupt file is quarantined aside, never a boot loop") {
    RequireSodium();
    const auto path = TempFile("clipp-test-registers-corrupt.bin");
    CleanupSnapshotFiles(path);
    {
        std::ofstream garbage(path, std::ios::binary);
        garbage << "this is not a sealed snapshot";
    }

    std::vector<RegisterRecord> out;
    CHECK(RegisterPersistence::LoadSnapshotFile(path.string(), MakeKey(0x42), out)
          == RegisterPersistence::LoadResult::Corrupt);
    // Original gone, carcass present: the next boot sees NoFile and starts clean.
    CHECK_FALSE(std::filesystem::exists(path));
    std::filesystem::path corrupt = path;
    corrupt += ".corrupt";
    CHECK(std::filesystem::exists(corrupt));
    CHECK(RegisterPersistence::LoadSnapshotFile(path.string(), MakeKey(0x42), out)
          == RegisterPersistence::LoadResult::NoFile);

    CleanupSnapshotFiles(path);
}

TEST_CASE("loaded snapshot merges into a store via ApplyRemote") {
    RequireSodium();
    const auto key = MakeKey(0x42);
    const auto blob = RegisterPersistence::SealSnapshot(SampleRecords(), key);
    std::vector<RegisterRecord> out;
    REQUIRE(RegisterPersistence::TryOpenSnapshot(blob, key, out));

    // Fake now just past the sample stamps: the records must be within TTL,
    // or ApplyRemote (correctly) refuses them as already dead.
    RegisterStore store(MakeHost(9), RegisterStore::kDefaultTtlMs,
                        RegisterStore::kDefaultMaxCount,
                        RegisterStore::kDefaultMaxValueBytes,
                        [] { return 1'700'000'002'000ull; });
    size_t applied = 0;
    for (auto& record : out) {
        if (store.ApplyRemote(std::move(record))) {
            ++applied;
        }
    }
    CHECK(applied == SampleRecords().size());
    // Live values only — the tombstone stays invisible to List().
    CHECK(store.List().size() == SampleRecords().size() - 1);
    // The store clock witnessed every persisted stamp.
    CHECK(store.ClockHighWater().wallMs >= 1'700'000'000'123ull);
}

TEST_CASE("store change listener fires on replicated-state changes only") {
    RequireSodium();
    // Fake now keeps the 2023-stamped ApplyRemote record inside the TTL.
    RegisterStore store(MakeHost(9), RegisterStore::kDefaultTtlMs,
                        RegisterStore::kDefaultMaxCount,
                        RegisterStore::kDefaultMaxValueBytes,
                        [] { return 1'700'000'002'000ull; });
    int fires = 0;
    store.AddChangeListener([&fires] { ++fires; });

    CHECK(store.Upsert("a", "1") == RegisterStore::WriteResult::Ok);
    CHECK(fires == 1);
    CHECK(store.Read("a").has_value());  // touched bump counts
    CHECK(fires == 2);
    CHECK(store.Delete("a") == RegisterStore::DeleteResult::Deleted);
    CHECK(fires == 3);
    CHECK(store.Delete("a") == RegisterStore::DeleteResult::NotFound);  // no-op: silent
    CHECK(fires == 3);
    CHECK_FALSE(store.Read("absent").has_value());  // miss: silent
    CHECK(fires == 3);
    store.MirrorDefault("clipboard text");  // mirror: never persisted, silent
    CHECK(fires == 3);
    CHECK(store.ApplyRemote(MakeRec("remote", "value", 0)));
    CHECK(fires == 4);
    CHECK_FALSE(store.ApplyRemote(MakeRec("remote", "value", 0)));  // dominated: silent
    CHECK(fires == 4);

    // Listeners are append-only and ALL fire — iOS arms two (persistence dirty
    // hook + UI refresh), so a second registration must not displace the first.
    int secondFires = 0;
    store.AddChangeListener([&secondFires] { ++secondFires; });
    CHECK(store.Upsert("b", "2") == RegisterStore::WriteResult::Ok);
    CHECK(fires == 5);
    CHECK(secondFires == 1);
}
