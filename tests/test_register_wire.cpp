#include <doctest/doctest.h>

#include "Hlc.h"
#include "HostId.h"
#include "RegisterStore.h"
#include "RegisterWire.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

HostId MakeHost(uint8_t tag) {
    HostId::Bytes b{};
    b[0] = tag;
    b[15] = 0xAB;  // a non-zero tail so a dropped/!zeroed byte would show up
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

}  // namespace

TEST_CASE("REGW record round-trips through encode/decode") {
    SUBCASE("value record carries transport flags") {
        const RegisterRecord in = MakeRec("url", "https://example.com", 0);
        const auto body = RegisterWire::EncodeRecord(in, RegisterWire::Transport::Relay);
        RegisterRecord out;
        uint8_t transport = 0;
        REQUIRE(RegisterWire::TryDecodeRecord(body, out, transport));
        CHECK(out == in);
        CHECK(transport == RegisterWire::Transport::Relay);
    }
    SUBCASE("tombstone: empty value + flag") {
        const RegisterRecord in = MakeRec("gone", "", RegisterFlags::Tombstone);
        const auto body = RegisterWire::EncodeRecord(in, 0);
        RegisterRecord out;
        uint8_t transport = 7;
        REQUIRE(RegisterWire::TryDecodeRecord(body, out, transport));
        CHECK(out == in);
        CHECK(out.IsTombstone());
        CHECK(transport == 0);
    }
    SUBCASE("private flag preserved") {
        const RegisterRecord in = MakeRec("pw", "s3cret", RegisterFlags::Private);
        const auto body = RegisterWire::EncodeRecord(in, 0);
        RegisterRecord out;
        uint8_t transport = 0;
        REQUIRE(RegisterWire::TryDecodeRecord(body, out, transport));
        CHECK(out == in);
        CHECK(out.IsPrivate());
    }
    SUBCASE("max-length name, empty value") {
        const RegisterRecord in = MakeRec(std::string(64, 'a'), "", 0);
        const auto body = RegisterWire::EncodeRecord(in, 0);
        RegisterRecord out;
        uint8_t transport = 0;
        REQUIRE(RegisterWire::TryDecodeRecord(body, out, transport));
        CHECK(out == in);
    }
}

TEST_CASE("REGW decode rejects malformed frames") {
    const RegisterRecord in = MakeRec("url", "value", 0);
    const auto good = RegisterWire::EncodeRecord(in, 0);
    RegisterRecord out;
    uint8_t transport = 0;

    // Any prefix-truncation is rejected (no read runs off the end).
    for (size_t cut = 0; cut < good.size(); ++cut) {
        const std::vector<unsigned char> truncated(good.begin(), good.begin() + cut);
        CHECK_FALSE(RegisterWire::TryDecodeRecord(truncated, out, transport));
    }
    // Trailing garbage.
    std::vector<unsigned char> extra = good;
    extra.push_back(0xFF);
    CHECK_FALSE(RegisterWire::TryDecodeRecord(extra, out, transport));
    // Unknown version.
    std::vector<unsigned char> badVersion = good;
    badVersion[0] = 99;
    CHECK_FALSE(RegisterWire::TryDecodeRecord(badVersion, out, transport));
    // Empty.
    CHECK_FALSE(RegisterWire::TryDecodeRecord({}, out, transport));
}

TEST_CASE("RSYN digest round-trips and rejects malformed") {
    const std::vector<RegisterDigestEntry> in = {
        { "alpha", Hlc{ 10, 1 }, Hlc{ 11, 0 } },
        { "beta", Hlc{ 20, 0 }, Hlc{ 25, 3 } },
        { std::string(64, 'z'), Hlc{ 30, 0 }, Hlc{ 30, 0 } },
    };
    const auto body = RegisterWire::EncodeDigest(in);
    std::vector<RegisterDigestEntry> out;
    REQUIRE(RegisterWire::TryDecodeDigest(body, out));
    REQUIRE(out.size() == in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        CHECK(out[i].name == in[i].name);
        CHECK(out[i].written == in[i].written);
        CHECK(out[i].touched == in[i].touched);
    }

    // Empty digest round-trips and clears the output.
    const auto emptyBody = RegisterWire::EncodeDigest({});
    std::vector<RegisterDigestEntry> emptyOut = { { "junk", {}, {} } };
    REQUIRE(RegisterWire::TryDecodeDigest(emptyBody, emptyOut));
    CHECK(emptyOut.empty());

    // Truncation is rejected.
    for (size_t cut = 0; cut < body.size(); ++cut) {
        const std::vector<unsigned char> truncated(body.begin(), body.begin() + cut);
        CHECK_FALSE(RegisterWire::TryDecodeDigest(truncated, out));
    }
}

TEST_CASE("RGET name round-trips and rejects malformed") {
    const auto body = RegisterWire::EncodeName("my.register-1");
    std::string out;
    REQUIRE(RegisterWire::TryDecodeName(body, out));
    CHECK(out == "my.register-1");

    std::vector<unsigned char> extra = body;
    extra.push_back(0);
    CHECK_FALSE(RegisterWire::TryDecodeName(extra, out));
    CHECK_FALSE(RegisterWire::TryDecodeName({}, out));
}
