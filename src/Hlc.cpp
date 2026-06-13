#include "Hlc.h"

#include <algorithm>
#include <chrono>
#include <utility>

std::array<uint8_t, 16> Hlc::Pack() const {
    std::array<uint8_t, 16> out{};
    for (size_t i = 0; i < 8; ++i) {
        out[i]     = static_cast<uint8_t>((wallMs  >> (56 - 8 * i)) & 0xFF);
        out[8 + i] = static_cast<uint8_t>((counter >> (56 - 8 * i)) & 0xFF);
    }
    return out;
}

Hlc Hlc::Unpack(const std::array<uint8_t, 16>& bytes) {
    Hlc h;
    for (size_t i = 0; i < 8; ++i) {
        h.wallMs  = (h.wallMs  << 8) | bytes[i];
        h.counter = (h.counter << 8) | bytes[8 + i];
    }
    return h;
}

uint64_t HlcClock::SystemNowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

HlcClock::HlcClock(std::function<uint64_t()> physicalNowMs)
    : physicalNowMs_(std::move(physicalNowMs)) {}

Hlc HlcClock::Now() {
    const uint64_t phys = physicalNowMs_();
    if (phys > last_.wallMs) {
        last_ = Hlc{ phys, 0 };
    } else {
        // Wall clock stalled, regressed, or two events share a millisecond: hold
        // the high-water wall reading and advance the logical counter so the new
        // HLC still sorts strictly after the previous one.
        last_ = Hlc{ last_.wallMs, last_.counter + 1 };
    }
    return last_;
}

void HlcClock::Witness(const Hlc& seen) {
    last_ = std::max(last_, seen);
}

void HlcClock::SeedFloor(const Hlc& floor) {
    last_ = std::max(last_, floor);
}
