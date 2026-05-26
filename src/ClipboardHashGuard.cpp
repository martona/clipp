#include "ClipboardHashGuard.h"

#include <cstring>

bool ClipboardHashGuard::IsCurrent(const ClipboardPayload& payload) {
    // hashAlg == 0 means the payload arrived without a hash. We can't dedup
    // against an unknown — treat as "not current" so the caller proceeds.
    if (payload.meta.hashAlg == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!currentValid_) {
        return false;
    }
    if (currentHashAlg_ != payload.meta.hashAlg) {
        return false;
    }
    return std::memcmp(currentHash_.data(), payload.meta.hashBytes, currentHash_.size()) == 0;
}

void ClipboardHashGuard::RememberCurrent(const ClipboardPayload& payload) {
    if (payload.meta.hashAlg == 0) {
        // Without a hash we have nothing meaningful to remember; invalidate so
        // subsequent IsCurrent calls don't accidentally hit a stale match.
        std::lock_guard<std::mutex> lock(mutex_);
        currentValid_ = false;
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    currentValid_ = true;
    currentHashAlg_ = payload.meta.hashAlg;
    std::memcpy(currentHash_.data(), payload.meta.hashBytes, currentHash_.size());
}

bool ClipboardHashGuard::AcceptCurrent(const ClipboardPayload& payload) {
    if (payload.meta.hashAlg == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        currentValid_ = false;
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (currentValid_
        && currentHashAlg_ == payload.meta.hashAlg
        && std::memcmp(currentHash_.data(), payload.meta.hashBytes, currentHash_.size()) == 0) {
        return false;
    }
    currentValid_ = true;
    currentHashAlg_ = payload.meta.hashAlg;
    std::memcpy(currentHash_.data(), payload.meta.hashBytes, currentHash_.size());
    return true;
}
