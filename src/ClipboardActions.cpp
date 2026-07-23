#include "platform.h"

#include "RegisterConfig.h"

// Desktop daemon builds only — the same population as the register daemon
// (headless Linux runs no daemon; iOS has neither ClippPage nor a popup and
// receives its clipboard through the bridge stub).
#if CLIPP_REGISTERS_DAEMON

#include "ClipboardActions.h"

#include "Clipboard.h"
#include "ClipboardActivityStore.h"
#include "ClipboardFlowUi.h"
#include "PeerManager.h"
#include "RegisterStore.h"
#include "Settings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

extern PeerManager g_peerManager;

namespace clipp {

void MirrorClipboardToDefaultRegister(const std::shared_ptr<const ClipboardPayload>& payload) {
    if (!payload || !IsClippTextFormat(payload->meta.formatId)) {
        return;
    }
    const std::vector<unsigned char>* bytes = payload->TryGetUncompressedBytes();
    if (bytes == nullptr) {
        return;
    }
    // Clipboard text carries a convention NUL terminator (see SetUncompressedBytes / capture).
    // Mirror the logical content -- what `clipp paste` emits -- so `ls` reports the same length
    // as a named register instead of counting the terminator.
    size_t n = bytes->size();
    if (n > 0 && bytes->back() == '\0') --n;
    g_registerStore.MirrorDefault(std::string(bytes->begin(), bytes->begin() + n));
    g_settings.noteRegisterHlcWallMs(g_registerStore.ClockHighWater().wallMs);
}

bool ReshareActivityItem(uint64_t itemID) {
    const auto stored = g_clipboardActivityStore.PayloadReference(itemID);
    if (!stored) {
        return false;
    }

    // Source-marked-private placeholders carry no content and exist only to
    // inform the user something happened — nothing to share.
    if ((stored->meta.flags & NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE) != 0
        && stored->EncodedBytes().empty()) {
        return false;
    }

    auto clone = std::make_shared<ClipboardPayload>();
    clone->meta = stored->meta;
    // Transport flags don't survive a re-share: SYNC_REPLAY marks historical
    // catch-up (this is a live event) and RELAY belongs to one-shot intake.
    // SOURCE_MARKED_PRIVATE is content truth and stays.
    clone->meta.flags &= ~(NetworkDefs::CLPM_FLAG_SYNC_REPLAY | NetworkDefs::CLPM_FLAG_RELAY);
    // Strictly above the stored stamp: the relocate rule (ours and every
    // peer's) fires only on a strictly newer timestamp, and a skewed origin
    // clock may sit ahead of our wall clock.
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    clone->meta.timestamp = (std::max)(nowMs, stored->meta.timestamp + 1);
    clone->SetEncodedBytes(std::vector<unsigned char>(stored->EncodedBytes()));

    std::shared_ptr<const ClipboardPayload> payload = std::move(clone);

    // Order matters: write the OS clipboard first — markAsClippOriginated arms
    // the hash guard (RememberCurrent), so the watcher treats the resulting
    // clipboard-change notification as an echo instead of re-stamping it as a
    // brand-new locally-originated event.
    SetClipboardData(payload, true);
    g_clipboardActivityStore.Add(payload);  // same guid + newer ts -> relocate
    MirrorClipboardToDefaultRegister(payload);
    const size_t queuedToPeers = g_peerManager.BroadcastClipboard(payload);
    if (queuedToPeers > 0) {
        // Same honesty rule as a fresh copy: the "sent" nudge fires only when
        // the item was actually handed to a peer connection.
        NotifyClipboardFlow(ClipboardFlowDirection::Sent, {});
    }
    return true;
}

bool DeleteActivityItemEverywhere(uint64_t itemID) {
    const auto stored = g_clipboardActivityStore.PayloadReference(itemID);
    if (!stored) {
        return false;
    }

    std::array<uint8_t, 16> guid{};
    std::memcpy(guid.data(), stored->meta.eventGuid, guid.size());

    if (!g_clipboardActivityStore.Remove(itemID)) {
        return false;
    }

    const bool guidIsZero = std::all_of(guid.begin(), guid.end(),
        [](uint8_t b) { return b == 0; });
    if (!guidIsZero) {
        g_peerManager.BroadcastFrame({ 'C', 'D', 'E', 'L' },
            std::vector<unsigned char>(guid.begin(), guid.end()));
    }
    return true;
}

}  // namespace clipp

#endif  // CLIPP_REGISTERS_DAEMON
