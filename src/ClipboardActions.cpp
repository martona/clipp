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
#include "ClipboardFormat.h"
#include "CryptoChannel.h"
#include "LocalPeerName.h"
#include "PeerManager.h"
#include "RegisterStore.h"
#include "RegisterWire.h"
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

namespace {

// The apply tail every locally-driven clipboard event runs (a re-share, a
// register made current). Order matters: the OS clipboard write goes first —
// markAsClippOriginated arms the hash guard (RememberCurrent), so the watcher
// treats the resulting clipboard-change notification as an echo instead of
// re-stamping it as a brand-new locally-originated event.
void ApplyAndBroadcastPayload(const std::shared_ptr<const ClipboardPayload>& payload) {
    SetClipboardData(payload, true);
    g_clipboardActivityStore.Add(payload);
    MirrorClipboardToDefaultRegister(payload);
    const size_t queuedToPeers = g_peerManager.BroadcastClipboard(payload);
    if (queuedToPeers > 0) {
        // Same honesty rule as a fresh copy: the "sent" nudge fires only when
        // the item was actually handed to a peer connection.
        NotifyClipboardFlow(ClipboardFlowDirection::Sent, {});
    }
}

// The mesh half of a local register write — exactly what the gateway does
// after re-stamping a one-shot client's record (Peer.cpp REGW-relay): persist
// the HLC floor, then push the record now in the store (value or tombstone)
// to register-serving peers.
void BroadcastRegisterRecord(const std::string& name) {
    g_settings.noteRegisterHlcWallMs(g_registerStore.ClockHighWater().wallMs);
    if (const auto stored = g_registerStore.GetForBroadcast(name)) {
        const auto regw = RegisterWire::EncodeRecord(*stored, 0);
        g_peerManager.BroadcastRegisterFrame({ 'R', 'E', 'G', 'W' }, regw);
    }
}

}  // namespace

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

    // The store Add inside sees the same guid with a newer ts -> relocate.
    ApplyAndBroadcastPayload(std::move(clone));
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

bool MakeRegisterCurrent(const std::string& name) {
    const auto rec = g_registerStore.Read(name);  // touch side effect, like `paste`
    if (!rec.has_value()) {
        return false;
    }

    // The same item `clipp copy` would send: canonical text plus the capture-
    // convention trailing NUL — or, for a binary register, the raw stream with
    // the header's format. Mirrors the gateway's RPUT build.
    ClipboardPayload payload;
    std::vector<unsigned char> bytes;
    if (rec->IsBinary()) {
        RegisterWire::BinaryValueInfo info{};
        if (!RegisterWire::TryParseBinaryValue(rec->value, info)) {
            return false;  // flagged but unparseable: refuse rather than share garbage
        }
        payload.meta.formatId = info.formatId;
        bytes.assign(rec->value.begin() + static_cast<std::ptrdiff_t>(info.streamOffset),
            rec->value.end());
    } else {
        payload.meta.formatId = CLIPP_FORMAT_UTF8;
        bytes.assign(rec->value.begin(), rec->value.end());
        bytes.push_back('\0');
    }
    if (!payload.SetUncompressedBytes(std::move(bytes))) {
        return false;
    }
    if (rec->IsPrivate()) {
        // Register privacy is content truth: ride the event as the same marker
        // a source app would set, so every activity list masks the preview.
        payload.meta.flags |= NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE;
    }

    // Fresh origin, fresh eventGuid: making a register current is a NEW
    // clipboard event originated here, not a re-share of a stored one.
    HostId localHost{};
    g_settings.getHostID(localHost);
    payload.StampOrigin(localHost,
        GetLocalPeerDisplayName("", CryptoChannel::HOSTNAME_MAX_BYTES).c_str(),
        g_settings.nextOriginSequenceNumber());

    ApplyAndBroadcastPayload(std::make_shared<const ClipboardPayload>(std::move(payload)));
    return true;
}

bool SaveActivityItemAsRegister(uint64_t itemID, const std::string& name, bool markPrivate) {
    const auto stored = g_clipboardActivityStore.PayloadReference(itemID);
    if (!stored) {
        return false;
    }
    const std::vector<unsigned char>* bytes = stored->TryGetUncompressedBytes();
    if (bytes == nullptr || bytes->empty()) {
        return false;  // private placeholders and undecodable payloads carry nothing to save
    }

    std::string value;
    uint8_t flags = markPrivate ? RegisterFlags::Private : uint8_t{ 0 };
    if (IsClippTextFormat(stored->meta.formatId)) {
        // Logical content, like the "" mirror: the trailing NUL is transport
        // convention, not text.
        size_t n = bytes->size();
        if (n > 0 && bytes->back() == '\0') --n;
        value.assign(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(n));
    } else if (IsClippImageFormat(stored->meta.formatId)) {
        value = RegisterWire::EncodeBinaryValue(stored->meta.formatId, bytes->data(), bytes->size());
        flags |= RegisterFlags::BinaryHeader;
    } else {
        return false;
    }

    HostId localHost{};
    g_settings.getHostID(localHost);
    if (g_registerStore.UpsertWithFlags(name, std::move(value), flags, localHost)
        != RegisterStore::WriteResult::Ok) {
        return false;
    }
    BroadcastRegisterRecord(name);
    return true;
}

bool DeleteRegisterEverywhere(const std::string& name) {
    if (g_registerStore.Delete(name) != RegisterStore::DeleteResult::Deleted) {
        return false;
    }
    BroadcastRegisterRecord(name);  // GetForBroadcast surfaces the fresh tombstone
    return true;
}

bool RenameRegister(const std::string& oldName, const std::string& newName) {
    if (newName == oldName) {
        return true;
    }
    // The GUI editor pre-validates (and refuses collisions); re-check the
    // contract here anyway. A rename onto an existing name is a legitimate
    // LWW overwrite at this layer.
    if (!IsValidRegisterName(newName)) {
        return false;
    }
    auto rec = g_registerStore.Read(oldName);  // live values only; the touch is harmless
    if (!rec.has_value()) {
        return false;
    }

    HostId localHost{};
    g_settings.getHostID(localHost);
    const uint8_t valueFlags = static_cast<uint8_t>(
        rec->flags & (RegisterFlags::Private | RegisterFlags::BinaryHeader));
    if (g_registerStore.UpsertWithFlags(newName, std::move(rec->value), valueFlags, localHost)
        != RegisterStore::WriteResult::Ok) {
        return false;
    }
    BroadcastRegisterRecord(newName);
    if (g_registerStore.Delete(oldName) == RegisterStore::DeleteResult::Deleted) {
        BroadcastRegisterRecord(oldName);
    }
    return true;
}

}  // namespace clipp

#endif  // CLIPP_REGISTERS_DAEMON
