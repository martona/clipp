#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <functional>

// Hybrid logical clock: a 64-bit wall-clock millisecond reading paired with a
// 64-bit logical counter. The pair is totally ordered (wallMs, then counter); the
// counter advances when two events fall in the same wall millisecond — or when a
// stalled/regressed wall clock would otherwise tie — so causality, not raw wall
// time, breaks ties within a millisecond. Wall clocks alone are disqualified for
// LWW conflict resolution: cross-device skew would let a lagging clock win.
struct Hlc {
    uint64_t wallMs{ 0 };
    uint64_t counter{ 0 };

    // Total order over (wallMs, counter); synthesizes ==, !=, <, <=, >, >=.
    auto operator<=>(const Hlc&) const = default;

    // Endianness-independent 16-byte encoding (wallMs then counter, big-endian) so
    // the same bytes compare identically on every platform on the wire.
    std::array<uint8_t, 16> Pack() const;
    static Hlc Unpack(const std::array<uint8_t, 16>& bytes);
};

// Generates monotonically non-decreasing HLCs for locally originated events and
// ratchets past HLCs observed from peers. Not thread-safe on its own —
// RegisterStore owns one behind its own mutex.
class HlcClock {
public:
    // physicalNowMs supplies wall-clock milliseconds since the Unix epoch.
    // Injectable so tests can drive skew and elapsed time deterministically.
    explicit HlcClock(std::function<uint64_t()> physicalNowMs = &HlcClock::SystemNowMs);

    // Fresh HLC for a locally originated event: max(physical now, high-water) with
    // the counter bumped on a tie. Never regresses below anything generated or
    // witnessed.
    Hlc Now();

    // Fold a peer's HLC into local state so subsequent Now() values sit strictly
    // above it. Call for both HLCs carried on an inbound record before merging it.
    void Witness(const Hlc& seen);

    // Seed the high-water mark from the persisted floor on startup so HLCs survive
    // a restart (and a regressed wall clock) without reissuing values. See the
    // OriginSequenceFloor precedent in Settings.
    void SeedFloor(const Hlc& floor);

    Hlc HighWater() const { return last_; }

    static uint64_t SystemNowMs();

private:
    Hlc last_{};
    std::function<uint64_t()> physicalNowMs_;
};
