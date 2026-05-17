#include "ClipboardHashGuard.h"
#include <xxhash.h>

ClipboardHashGuard::Fingerprint ClipboardHashGuard::ComputeFingerprint(const ClipboardPayload& payload) {
    const XXH128_hash_t hash = XXH3_128bits_withSeed(
        payload.rawData.data(),
        payload.rawData.size(),
        static_cast<XXH64_hash_t>(payload.formatId));
    return { hash.high64, hash.low64 };
}

bool ClipboardHashGuard::SameFingerprint(const Fingerprint& lhs, const Fingerprint& rhs) {
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool ClipboardHashGuard::IsCurrent(const ClipboardPayload& payload) {
    const Fingerprint fingerprint = ComputeFingerprint(payload);
    std::lock_guard<std::mutex> lock(mutex_);
    return currentFingerprintValid_ &&
        SameFingerprint(fingerprint, currentFingerprint_);
}

void ClipboardHashGuard::RememberCurrent(const ClipboardPayload& payload) {
    const Fingerprint fingerprint = ComputeFingerprint(payload);
    std::lock_guard<std::mutex> lock(mutex_);
    currentFingerprint_ = fingerprint;
    currentFingerprintValid_ = true;
}

bool ClipboardHashGuard::AcceptCurrent(const ClipboardPayload& payload) {
    const Fingerprint fingerprint = ComputeFingerprint(payload);
    std::lock_guard<std::mutex> lock(mutex_);
    if (currentFingerprintValid_ &&
        SameFingerprint(fingerprint, currentFingerprint_)) {
        return false;
    }

    currentFingerprint_ = fingerprint;
    currentFingerprintValid_ = true;
    return true;
}
