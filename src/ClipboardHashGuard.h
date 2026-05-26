#pragma once

#include "platform.h"
#include "ClipboardPayload.h"

#include <array>
#include <cstdint>
#include <mutex>

// Tracks the clipboard payload Clipp currently accepts as local state.
// This is separate from platform self-origin suppression: Windows has an origin
// marker, and macOS fast-forwards pasteboard changeCount after writes.
// Third-party sync agents such as RDP, VM clipboard sharing, or cloud clipboard
// can reintroduce the same payload without preserving those platform signals.
// The guard suppresses those content echoes by comparing the payload's
// meta.hashBytes (XXH3_128 of the plaintext, seeded by formatId, computed once
// at SetUncompressedBytes/FinalizeOutgoingPayload time).
// Local reads call AcceptCurrent; remote writes call IsCurrent/RememberCurrent.
// It deliberately models only one atomic clipboard state, not a history.
class ClipboardHashGuard {
public:
    bool IsCurrent(const ClipboardPayload& payload);
    void RememberCurrent(const ClipboardPayload& payload);
    bool AcceptCurrent(const ClipboardPayload& payload);

private:
    std::mutex mutex_;
    bool currentValid_{ false };
    uint8_t currentHashAlg_{ 0 };
    std::array<uint8_t, sizeof(NetworkDefs::ClipboardMessage::hashBytes)> currentHash_{};
};
