// Binary registers (RegisterFlags::BinaryHeader) + the wide GUI-era name
// validator: header round-trips, byte-exact transit of binary values through
// the record wire format, flag preservation through the store, and the
// accept/reject matrix for IsValidRegisterName.

#include <doctest/doctest.h>

#include "ClipboardFormat.h"
#include "RegisterStore.h"
#include "RegisterWire.h"

#include <cstring>
#include <string>

TEST_CASE("wide register names: accept/reject matrix") {
    // The old CLI charset stays valid.
    CHECK(IsValidRegisterName("url"));
    CHECK(IsValidRegisterName("a.b_c-d"));
    CHECK(IsValidRegisterName("item-1"));
    // GUI-era names.
    CHECK(IsValidRegisterName("My Stuff!"));
    CHECK(IsValidRegisterName("a b"));                       // interior space
    CHECK(IsValidRegisterName("\xC3\x9Crl"));                // U+00DC 'Ü'
    CHECK(IsValidRegisterName("\xF0\x9F\x9A\x80 launch"));   // 4-byte emoji
    CHECK(IsValidRegisterName(std::string(64, 'a')));        // 64 bytes exactly

    // Size / emptiness.
    CHECK_FALSE(IsValidRegisterName(""));
    CHECK_FALSE(IsValidRegisterName(std::string(65, 'a')));
    // The three reserved printables.
    CHECK_FALSE(IsValidRegisterName("a/b"));
    CHECK_FALSE(IsValidRegisterName("a?b"));
    CHECK_FALSE(IsValidRegisterName("a*b"));
    // Controls: C0, DEL, C1.
    CHECK_FALSE(IsValidRegisterName("a\nb"));
    CHECK_FALSE(IsValidRegisterName("a\tb"));
    CHECK_FALSE(IsValidRegisterName(std::string("a\x7F" "b")));
    CHECK_FALSE(IsValidRegisterName("a\xC2\x80" "b"));       // C1 U+0080
    CHECK_FALSE(IsValidRegisterName("a\xC2\x9F" "b"));       // C1 U+009F
    // Edge whitespace.
    CHECK_FALSE(IsValidRegisterName(" a"));
    CHECK_FALSE(IsValidRegisterName("a "));
    CHECK_FALSE(IsValidRegisterName(" "));
    // Malformed UTF-8: stray continuation, truncation, overlong, surrogate.
    CHECK_FALSE(IsValidRegisterName("\xFF"));
    CHECK_FALSE(IsValidRegisterName("\xC3"));
    CHECK_FALSE(IsValidRegisterName("\xC0\xAF"));            // overlong '/'
    CHECK_FALSE(IsValidRegisterName("\xED\xA0\x80"));        // UTF-8'd surrogate
}

TEST_CASE("binary value header: round-trip, forward seek, bounds") {
    const unsigned char stream[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0xFF };
    const std::string value =
        RegisterWire::EncodeBinaryValue(CLIPP_FORMAT_PNG, stream, sizeof(stream));
    CHECK(value.size() == RegisterWire::kBinaryHeaderV1Size + sizeof(stream));

    RegisterWire::BinaryValueInfo info{};
    REQUIRE(RegisterWire::TryParseBinaryValue(value, info));
    CHECK(info.formatId == CLIPP_FORMAT_PNG);
    CHECK(info.streamOffset == RegisterWire::kBinaryHeaderV1Size);
    CHECK(std::memcmp(value.data() + info.streamOffset, stream, sizeof(stream)) == 0);

    // An RLST preview carrying only the header still parses.
    REQUIRE(RegisterWire::TryParseBinaryValue(
        value.substr(0, RegisterWire::kBinaryHeaderV1Size), info));
    CHECK(info.formatId == CLIPP_FORMAT_PNG);

    // A future, longer header: readers seek past unknown trailing fields.
    std::string future = value;
    future.insert(RegisterWire::kBinaryHeaderV1Size, 4, '\x00');
    future[2] = 0;
    future[3] = static_cast<char>(RegisterWire::kBinaryHeaderV1Size + 4);
    REQUIRE(RegisterWire::TryParseBinaryValue(future, info));
    CHECK(info.streamOffset == RegisterWire::kBinaryHeaderV1Size + 4);
    CHECK(std::memcmp(future.data() + info.streamOffset, stream, sizeof(stream)) == 0);

    // Malformed: too short, headerLen below the fixed size, headerLen past the end.
    CHECK_FALSE(RegisterWire::TryParseBinaryValue(std::string(), info));
    CHECK_FALSE(RegisterWire::TryParseBinaryValue(std::string(4, 'x'), info));
    std::string bad = value;
    bad[2] = 0;
    bad[3] = 4;
    CHECK_FALSE(RegisterWire::TryParseBinaryValue(bad, info));
    bad = value.substr(0, RegisterWire::kBinaryHeaderV1Size);
    bad[2] = 0x7F;
    bad[3] = 0;
    CHECK_FALSE(RegisterWire::TryParseBinaryValue(bad, info));
}

TEST_CASE("binary record: wire round-trip preserves flag and bytes") {
    const unsigned char stream[] = { 0x00, 0x0D, 0x0A, 0x0D, 0x00, 0xFE };
    RegisterRecord rec;
    rec.name = "shot";
    rec.value = RegisterWire::EncodeBinaryValue(CLIPP_FORMAT_JPEG, stream, sizeof(stream));
    rec.flags = RegisterFlags::BinaryHeader;

    const auto body = RegisterWire::EncodeRecord(rec, 0);
    RegisterRecord decoded;
    uint8_t transport = 0;
    REQUIRE(RegisterWire::TryDecodeRecord(body, decoded, transport));
    CHECK(decoded == rec);
    CHECK(decoded.IsBinary());

    RegisterWire::BinaryValueInfo info{};
    REQUIRE(RegisterWire::TryParseBinaryValue(decoded.value, info));
    CHECK(info.formatId == CLIPP_FORMAT_JPEG);
    CHECK(decoded.value.size() - info.streamOffset == sizeof(stream));
}

TEST_CASE("store: UpsertWithFlags preserves BinaryHeader, masks Tombstone") {
    RegisterStore store;
    HostId host{};
    store.SetLocalHost(host);

    const unsigned char stream[] = { 0x01, 0x00, 0x02 };
    const std::string value =
        RegisterWire::EncodeBinaryValue(CLIPP_FORMAT_PNG, stream, sizeof(stream));

    CHECK(store.UpsertWithFlags("img", value,
                                RegisterFlags::BinaryHeader | RegisterFlags::Private,
                                host) == RegisterStore::WriteResult::Ok);
    auto rec = store.Read("img");
    REQUIRE(rec.has_value());
    CHECK(rec->IsBinary());
    CHECK(rec->IsPrivate());
    CHECK_FALSE(rec->IsTombstone());

    // A caller can never smuggle a tombstone through the value-flag path.
    CHECK(store.UpsertWithFlags("img", value,
                                RegisterFlags::Tombstone | RegisterFlags::BinaryHeader,
                                host) == RegisterStore::WriteResult::Ok);
    rec = store.Read("img");
    REQUIRE(rec.has_value());
    CHECK(rec->IsBinary());
    CHECK_FALSE(rec->IsTombstone());

    // ApplyRemote keeps the winner's flags byte-for-byte.
    RegisterRecord incoming = *rec;
    incoming.written.counter += 1;
    incoming.touched.counter += 1;
    RegisterStore replica;
    replica.SetLocalHost(host);
    CHECK(replica.ApplyRemote(incoming));
    const auto replicated = replica.Read("img");
    REQUIRE(replicated.has_value());
    CHECK(replicated->IsBinary());

    // Wide names flow through the store's own validation.
    CHECK(store.Upsert("My Stuff!", "v") == RegisterStore::WriteResult::Ok);
    CHECK(store.Upsert("a/b", "v") == RegisterStore::WriteResult::InvalidName);
    CHECK(store.Upsert("a*b", "v") == RegisterStore::WriteResult::InvalidName);
}
