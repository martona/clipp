#include <doctest/doctest.h>

#include "Hlc.h"

#include <cstdint>

TEST_CASE("Hlc pack/unpack round-trips, big-endian") {
    const Hlc h{ 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull };
    const auto bytes = h.Pack();
    CHECK(Hlc::Unpack(bytes) == h);

    // Most-significant byte first, wallMs then counter.
    CHECK(bytes[0] == 0x01);
    CHECK(bytes[7] == 0xEF);
    CHECK(bytes[8] == 0xFE);
    CHECK(bytes[15] == 0x10);
}

TEST_CASE("Hlc total order is (wallMs, counter)") {
    CHECK(Hlc{ 1, 0 } < Hlc{ 1, 1 });
    CHECK(Hlc{ 1, 99 } < Hlc{ 2, 0 });
    CHECK(Hlc{ 2, 0 } > Hlc{ 1, 1000 });
    CHECK(Hlc{ 5, 5 } == Hlc{ 5, 5 });
}

TEST_CASE("HlcClock.Now is monotonic even when the wall clock regresses") {
    uint64_t fake = 1000;
    HlcClock clock([&fake] { return fake; });

    const Hlc a = clock.Now();   // {1000, 0}
    const Hlc b = clock.Now();   // same ms -> counter bumps
    CHECK(b > a);
    CHECK(b.wallMs == 1000);
    CHECK(b.counter == 1);

    fake = 500;                  // wall clock jumps backwards
    const Hlc c = clock.Now();
    CHECK(c > b);                // never regresses; holds the high-water wall

    fake = 2000;                 // wall advances past the high-water mark
    const Hlc d = clock.Now();
    CHECK(d > c);
    CHECK(d.wallMs == 2000);
    CHECK(d.counter == 0);       // fresh ms -> counter resets
}

TEST_CASE("HlcClock.Witness ratchets past a peer's clock") {
    uint64_t fake = 100;
    HlcClock clock([&fake] { return fake; });

    clock.Witness(Hlc{ 10000, 7 });
    const Hlc next = clock.Now();   // physical now (100) is far behind the witnessed 10000
    CHECK(next > Hlc{ 10000, 7 });
    CHECK(next.wallMs == 10000);
    CHECK(next.counter == 8);
}

TEST_CASE("HlcClock.SeedFloor lifts the clock without regressing it") {
    uint64_t fake = 100;
    HlcClock clock([&fake] { return fake; });
    clock.SeedFloor(Hlc{ 5000, 3 });
    CHECK(clock.HighWater() == Hlc{ 5000, 3 });
    clock.SeedFloor(Hlc{ 1, 0 });               // a lower floor must not pull it down
    CHECK(clock.HighWater() == Hlc{ 5000, 3 });
}
