#pragma once

#include "platform.h"
#include "ClipboardData.h"

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
    Link,
    Image,
};

struct ClipboardActivityItemHeader {
    uint64_t id{};
    ClipboardActivityDirection direction{ ClipboardActivityDirection::Incoming };
    std::wstring deviceName;
    std::chrono::system_clock::time_point timestamp{};
    uint32_t formatId{};
    size_t encodedBytes{};
    uint32_t uncompressedBytes{};
};

struct ClipboardActivityDisplayItem {
    ClipboardActivityItemHeader header;
    ClipboardActivityPayloadKind kind{ ClipboardActivityPayloadKind::Unsupported };
    std::wstring previewText;
    std::wstring detailText;
    std::wstring linkHost;
    std::vector<unsigned char> imagePngData;
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

    uint64_t AddIncoming(const std::wstring& deviceName, const ClipboardPayload& payload);
    uint64_t AddOutgoing(const std::wstring& deviceName, const ClipboardPayload& payload);

    void SetLimits(uint64_t memoryLimitBytes, uint64_t maxAgeSeconds, uint64_t maxItems);

    std::vector<ClipboardActivityItemHeader> Snapshot();
    std::optional<ClipboardActivityDisplayItem> DisplayItem(uint64_t itemID) const;
    std::optional<ClipboardPayload> PayloadForClipboard(uint64_t itemID) const;
    std::shared_ptr<const ClipboardPayload> PayloadReference(uint64_t itemID) const;
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

    static std::shared_ptr<const ClipboardPayload> MakeStoredPayload(const ClipboardPayload& payload);
    static std::optional<ClipboardActivityDisplayItem> BuildDisplayItem(const Item& item);
    static std::vector<ClipboardActivityItemHeader> SnapshotLocked(const std::vector<Item>& items);
    static uint64_t EstimateItemBytes(const Item& item);
    static void NotifyWatchers(const std::vector<WatcherRegistration>& watchers, const std::vector<ClipboardActivityUpdate>& updates);

    uint64_t AddItem(ClipboardActivityDirection direction, const std::wstring& deviceName, const ClipboardPayload& payload);
    void ApplyLimitsLocked(std::chrono::system_clock::time_point now, std::vector<ClipboardActivityUpdate>& updates);
    std::optional<Item> FindItem(uint64_t itemID) const;

    mutable std::mutex mutex_;
    std::vector<Item> items_;
    std::vector<WatcherRegistration> watchers_;
    Limits limits_;
    uint64_t nextItemID_{ 1 };
    std::size_t nextWatcherID_{ 1 };
};
