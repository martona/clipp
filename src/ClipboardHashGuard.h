#pragma once

#include "platform.h"
#include "ClipboardData.h"
#include <cstdint>
#include <mutex>

// Tracks the clipboard payload Clipp currently accepts as local state.
// This is separate from platform self-origin suppression: Windows has an origin
// marker, and macOS fast-forwards pasteboard changeCount after writes.
// Third-party sync agents such as RDP, VM clipboard sharing, or cloud clipboard
// can reintroduce the same payload without preserving those platform signals.
// The guard suppresses those content echoes by fingerprinting format + bytes.
// Local reads call AcceptCurrent; remote writes call IsCurrent/RememberCurrent.
// It deliberately models only one atomic clipboard state, not a history.
class ClipboardHashGuard {
public:
    bool IsCurrent(const ClipboardPayload& payload);
    void RememberCurrent(const ClipboardPayload& payload);
    bool AcceptCurrent(const ClipboardPayload& payload);

private:
    struct Fingerprint {
        uint64_t high{ 0 };
        uint64_t low{ 0 };
    };

    static Fingerprint ComputeFingerprint(const ClipboardPayload& payload);
    static bool SameFingerprint(const Fingerprint& lhs, const Fingerprint& rhs);

    std::mutex mutex_;
    bool currentFingerprintValid_{ false };
    Fingerprint currentFingerprint_{};
};
