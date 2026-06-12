#pragma once

#include "platform.h"
#include "ClipboardPayload.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

enum class ClipboardActivityDirection {
    Incoming,
    Outgoing,
};

enum class ClipboardActivityPayloadKind {
    Unsupported,
    Text,
    PrivateText,
    PrivatePlaceholder,
    Link,
    Image,
};

struct ClipboardActivityItemHeader {
    uint64_t id{};
    // Local wall-clock when the item was added to the store. Distinct from any
    // origin-device timestamp that may live in the payload's meta — pull that
    // via DisplayItem / PayloadReference if a consumer wants it.
    std::chrono::system_clock::time_point timestamp{};
};

struct ClipboardActivityDisplayItem {
    ClipboardActivityItemHeader header;
    // Resolved from the payload at build time: Outgoing iff originHostId matches
    // the local host, Incoming otherwise. deviceName is the localized "This
    // device" string when outgoing, the wire-carried origin hostname otherwise.
    ClipboardActivityDirection direction{ ClipboardActivityDirection::Incoming };
    std::wstring deviceName;
    ClipboardActivityPayloadKind kind{ ClipboardActivityPayloadKind::Unsupported };
    // True when the kind reflects an explicit OS-level privacy marker on the
    // payload (CLPM_FLAG_SOURCE_MARKED_PRIVATE), as opposed to the receive-side
    // single-line-no-whitespace heuristic. UI uses this to distinguish "the
    // marker propagated" from "we guessed" — e.g. by attaching a small badge.
    bool sourceMarked{ false };
    std::wstring previewText;
    // For PrivateText: the preview the item would have shown had it not been
    // masked. The UI's peek toggle swaps previewText for this. Empty for all
    // other kinds.
    std::wstring revealedPreviewText;
    std::wstring detailText;
    std::wstring linkHost;
    uint32_t imageFormatId{ CLIPP_FORMAT_NONE };
    // Aliasing shared_ptr to the encoded image bytes inside the stored ClipboardPayload.
    // Holding this keeps the payload alive without copying the bytes. Null for non-image items.
    std::shared_ptr<const std::vector<unsigned char>> imageData;
};

struct ClipboardActivityUpdate {
    enum class Type {
        Added,
        Removed,
        Cleared,
    };

    Type type{ Type::Added };
    uint64_t itemID{};
};

struct ClipboardActivityRegistration {
    std::size_t watcherID{};
    std::vector<ClipboardActivityItemHeader> items;
};

class ClipboardActivityStore {
public:
    using Watcher = std::function<void(const ClipboardActivityUpdate&, void*)>;

    // Insert a payload into the activity store. Direction and device-name are
    // derived at display time from payload->meta.originHostId / originHostName,
    // so the caller doesn't pass them in.
    uint64_t Add(std::shared_ptr<const ClipboardPayload> payload);

    void SetLimits(uint64_t memoryLimitBytes, uint64_t maxAgeSeconds, uint64_t maxItems);

    std::vector<ClipboardActivityItemHeader> Snapshot();
    std::optional<ClipboardActivityDisplayItem> DisplayItem(uint64_t itemID) const;
    std::shared_ptr<const ClipboardPayload> PayloadReference(uint64_t itemID) const;

    // The eventGuid of the most recent (highest meta.timestamp) item, or all
    // zeros if empty. Used by the sync-recovery requester to say "send me
    // everything you have after this point."
    std::array<uint8_t, 16> TailEventGuid() const;

    // Returns up to maxItems payloads strictly after the item identified by
    // fromGuid, in chronological order (oldest first), ready to be replayed as
    // sync items. If fromGuid is all zeros or not present in the store, returns
    // the most recent maxItems items instead (treating the requester as having
    // no anchor and falling back to "send me whatever you have").
    std::vector<std::shared_ptr<const ClipboardPayload>> ItemsSince(
        const std::array<uint8_t, 16>& fromGuid, uint64_t maxItems) const;
    bool Remove(uint64_t itemID);
    void Clear();

    ClipboardActivityRegistration QueryAndRegister(Watcher watcher, void* userData = nullptr);
    void Unregister(std::size_t watcherID);

private:
    struct Item {
        ClipboardActivityItemHeader header;
        std::shared_ptr<const ClipboardPayload> payload;
    };

    struct Limits {
        uint64_t memoryLimitBytes{ 256ull * 1024ull * 1024ull };
        uint64_t maxAgeSeconds{ 24ull * 60ull * 60ull };
        uint64_t maxItems{ 1000 };
    };

    struct WatcherRegistration {
        std::size_t watcherID{};
        Watcher watcher;
        void* userData{};
    };

    static std::optional<ClipboardActivityDisplayItem> BuildDisplayItem(const Item& item);
    static std::vector<ClipboardActivityItemHeader> SnapshotLocked(const std::vector<Item>& items);
    static uint64_t EstimateItemBytes(const Item& item);
    static void NotifyWatchers(const std::vector<WatcherRegistration>& watchers, const std::vector<ClipboardActivityUpdate>& updates);

    uint64_t AddItem(std::shared_ptr<const ClipboardPayload> payload);
    void ApplyLimitsLocked(std::chrono::system_clock::time_point now, std::vector<ClipboardActivityUpdate>& updates);
    std::optional<Item> FindItem(uint64_t itemID) const;

    mutable std::mutex mutex_;
    std::vector<Item> items_;
    std::vector<WatcherRegistration> watchers_;
    Limits limits_;
    uint64_t nextItemID_{ 1 };
    std::size_t nextWatcherID_{ 1 };
};

extern ClipboardActivityStore g_clipboardActivityStore;
