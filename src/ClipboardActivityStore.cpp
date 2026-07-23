#include "platform.h"

#include "ClipboardActivityStore.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

// Container half of the activity store: add/dedup/relocate, removal, limits,
// watchers, and the replay queries. Pure engine code — no g_settings, no
// uistrings, no text heuristics — so the unit tests compile it standalone.
// Display-item construction lives in ClipboardActivityDisplay.cpp.

uint64_t ClipboardActivityStore::Add(std::shared_ptr<const ClipboardPayload> payload) {
    return AddItem(std::move(payload));
}

void ClipboardActivityStore::SetLimits(uint64_t memoryLimitBytes, uint64_t maxAgeSeconds, uint64_t maxItems) {
    std::vector<ClipboardActivityUpdate> updates;
    std::vector<WatcherRegistration> watchers;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        limits_.memoryLimitBytes = memoryLimitBytes;
        limits_.maxAgeSeconds = maxAgeSeconds;
        limits_.maxItems = maxItems;
        ApplyLimitsLocked(std::chrono::system_clock::now(), updates);
        watchers = watchers_;
    }

    NotifyWatchers(watchers, updates);
}

std::vector<ClipboardActivityItemHeader> ClipboardActivityStore::Snapshot() {
    std::vector<ClipboardActivityItemHeader> snapshot;
    std::vector<ClipboardActivityUpdate> updates;
    std::vector<WatcherRegistration> watchers;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ApplyLimitsLocked(std::chrono::system_clock::now(), updates);
        snapshot = SnapshotLocked(items_);
        watchers = watchers_;
    }

    NotifyWatchers(watchers, updates);
    return snapshot;
}

std::shared_ptr<const ClipboardPayload> ClipboardActivityStore::PayloadReference(uint64_t itemID) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(items_.begin(), items_.end(), [itemID](const Item& item) {
        return item.header.id == itemID;
    });

    if (found == items_.end()) {
        return nullptr;
    }

    return found->payload;
}

bool ClipboardActivityStore::Remove(uint64_t itemID) {
    std::vector<ClipboardActivityUpdate> updates;
    std::vector<WatcherRegistration> watchers;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = std::find_if(items_.begin(), items_.end(), [itemID](const Item& item) {
            return item.header.id == itemID;
        });
        if (found == items_.end()) {
            return false;
        }

        items_.erase(found);
        updates.push_back({
            ClipboardActivityUpdate::Type::Removed,
            itemID,
        });
        watchers = watchers_;
    }

    NotifyWatchers(watchers, updates);

    return true;
}

bool ClipboardActivityStore::RemoveByEventGuid(const std::array<uint8_t, 16>& eventGuid) {
    const bool guidIsZero = std::all_of(eventGuid.begin(), eventGuid.end(),
        [](uint8_t b) { return b == 0; });
    if (guidIsZero) {
        return false;  // unstamped items are not addressable by wire identity
    }

    std::vector<ClipboardActivityUpdate> updates;
    std::vector<WatcherRegistration> watchers;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = std::find_if(items_.begin(), items_.end(), [&eventGuid](const Item& item) {
            return item.payload != nullptr &&
                std::memcmp(item.payload->meta.eventGuid, eventGuid.data(), eventGuid.size()) == 0;
        });
        if (found == items_.end()) {
            return false;
        }

        updates.push_back({
            ClipboardActivityUpdate::Type::Removed,
            found->header.id,
        });
        items_.erase(found);
        watchers = watchers_;
    }

    NotifyWatchers(watchers, updates);
    return true;
}

void ClipboardActivityStore::Clear() {
    std::vector<ClipboardActivityUpdate> updates;
    std::vector<WatcherRegistration> watchers;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) {
            return;
        }

        items_.clear();
        updates.push_back({
            ClipboardActivityUpdate::Type::Cleared,
            0,
        });
        watchers = watchers_;
    }

    NotifyWatchers(watchers, updates);
}

ClipboardActivityRegistration ClipboardActivityStore::QueryAndRegister(Watcher watcher, void* userData) {
    ClipboardActivityRegistration registration;
    std::vector<ClipboardActivityUpdate> updates;
    std::vector<WatcherRegistration> watchers;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        ApplyLimitsLocked(std::chrono::system_clock::now(), updates);
        registration.watcherID = nextWatcherID_++;
        registration.items = SnapshotLocked(items_);
        watchers = watchers_;
        watchers_.push_back({ registration.watcherID, std::move(watcher), userData });
    }

    NotifyWatchers(watchers, updates);
    return registration;
}

void ClipboardActivityStore::Unregister(std::size_t watcherID) {
    std::lock_guard<std::mutex> lock(mutex_);
    watchers_.erase(std::remove_if(watchers_.begin(), watchers_.end(), [watcherID](const WatcherRegistration& watcher) {
        return watcher.watcherID == watcherID;
    }), watchers_.end());
}

std::vector<ClipboardActivityItemHeader> ClipboardActivityStore::SnapshotLocked(const std::vector<Item>& items) {
    std::vector<ClipboardActivityItemHeader> snapshot;
    snapshot.reserve(items.size());
    for (const auto& item : items) {
        snapshot.push_back(item.header);
    }
    return snapshot;
}

uint64_t ClipboardActivityStore::EstimateItemBytes(const Item& item) {
    constexpr uint64_t kMetadataEstimateBytes = 256;
    const uint64_t payloadBytes = item.payload != nullptr
        ? static_cast<uint64_t>(item.payload->EncodedBytes().size())
        : 0;
    return kMetadataEstimateBytes + payloadBytes;
}

void ClipboardActivityStore::NotifyWatchers(
    const std::vector<WatcherRegistration>& watchers,
    const std::vector<ClipboardActivityUpdate>& updates)
{
    for (const auto& update : updates) {
        for (const auto& watcher : watchers) {
            if (watcher.watcher) {
                watcher.watcher(update, watcher.userData);
            }
        }
    }
}

uint64_t ClipboardActivityStore::AddItem(std::shared_ptr<const ClipboardPayload> payload) {
    if (!payload || payload->meta.formatId == CLIPP_FORMAT_NONE) {
        return 0;
    }

    uint64_t itemID = 0;
    std::vector<ClipboardActivityUpdate> updates;
    std::vector<WatcherRegistration> watchers;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Dedup by eventGuid. Same content from two peers, or our own event
        // bouncing back via sync, should result in a single stored item. An
        // all-zero GUID means "no GUID was assigned" (legacy / unstamped) —
        // skip dedup in that case since all such items would falsely collide.
        //
        // A known GUID with a strictly NEWER meta.timestamp is not an echo but
        // an MRU re-share ("move to top"): the same event was made current
        // again somewhere and re-stamped by the acting device. Relocate it:
        // replace the stored payload (so sync replay serves the bumped
        // timestamp onward) and re-insert at the new timestamp position,
        // keeping the item id stable — peek state and UI row identity key on
        // it. header.timestamp refreshes so age-based eviction and displayed
        // age track the re-share, not the original copy.
        const auto& guid = payload->meta.eventGuid;
        const bool guidIsZero = std::all_of(std::begin(guid), std::end(guid),
            [](uint8_t b) { return b == 0; });
        if (!guidIsZero) {
            for (auto existing = items_.begin(); existing != items_.end(); ++existing) {
                if (!existing->payload ||
                    std::memcmp(existing->payload->meta.eventGuid, guid, sizeof(guid)) != 0) {
                    continue;
                }

                if (payload->meta.timestamp <= existing->payload->meta.timestamp) {
                    return existing->header.id;
                }

                Item moved;
                moved.header.id = existing->header.id;
                moved.header.timestamp = std::chrono::system_clock::now();
                moved.payload = std::move(payload);
                itemID = moved.header.id;
                items_.erase(existing);

                const auto pos = std::lower_bound(items_.begin(), items_.end(), moved,
                    [](const Item& a, const Item& b) {
                        return a.payload->meta.timestamp < b.payload->meta.timestamp;
                    });
                items_.insert(pos, std::move(moved));

                updates.push_back({
                    ClipboardActivityUpdate::Type::Moved,
                    itemID,
                });
                break;
            }
        }

        if (itemID == 0) {
            Item item;
            item.header.id = nextItemID_++;
            item.header.timestamp = std::chrono::system_clock::now();
            item.payload = std::move(payload);

            itemID = item.header.id;
            updates.push_back({
                ClipboardActivityUpdate::Type::Added,
                itemID,
            });

            // Insert in ascending order of meta.timestamp so the stream reads
            // chronologically without the UI having to sort. Live items sit at the
            // tail (newest first in display), sync-replayed historical items slot
            // into their true position.
            const auto pos = std::lower_bound(items_.begin(), items_.end(), item,
                [](const Item& a, const Item& b) {
                    return a.payload->meta.timestamp < b.payload->meta.timestamp;
                });
            items_.insert(pos, std::move(item));
        }

        ApplyLimitsLocked(std::chrono::system_clock::now(), updates);
        watchers = watchers_;
    }

    NotifyWatchers(watchers, updates);

    return itemID;
}

std::array<uint8_t, 16> ClipboardActivityStore::TailEventGuid() const {
    std::array<uint8_t, 16> result{};
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty() || !items_.back().payload) {
        return result;
    }
    std::memcpy(result.data(), items_.back().payload->meta.eventGuid, result.size());
    return result;
}

std::vector<std::shared_ptr<const ClipboardPayload>> ClipboardActivityStore::ItemsSince(
    const std::array<uint8_t, 16>& fromGuid, uint64_t maxItems) const
{
    std::vector<std::shared_ptr<const ClipboardPayload>> result;
    if (maxItems == 0) {
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) {
        return result;
    }

    const bool guidIsZero = std::all_of(fromGuid.begin(), fromGuid.end(),
        [](uint8_t b) { return b == 0; });

    // Default starting index — used when fromGuid is zero/missing. Return the
    // most recent maxItems items (the tail of the store).
    size_t startIndex = 0;
    if (items_.size() > maxItems) {
        startIndex = items_.size() - static_cast<size_t>(maxItems);
    }

    if (!guidIsZero) {
        // Find fromGuid in the store. If present, start just after it. If not
        // present (peer's tail isn't in our window — maybe we evicted it, or we
        // never had it), fall through to "most recent N" behavior.
        for (size_t i = 0; i < items_.size(); ++i) {
            const auto& existing = items_[i];
            if (existing.payload &&
                std::memcmp(existing.payload->meta.eventGuid,
                            fromGuid.data(), fromGuid.size()) == 0) {
                startIndex = i + 1;
                break;
            }
        }
    }

    result.reserve(items_.size() - startIndex);
    for (size_t i = startIndex;
         i < items_.size() && result.size() < static_cast<size_t>(maxItems);
         ++i) {
        if (items_[i].payload) {
            result.push_back(items_[i].payload);
        }
    }
    return result;
}

void ClipboardActivityStore::ApplyLimitsLocked(
    std::chrono::system_clock::time_point now,
    std::vector<ClipboardActivityUpdate>& updates)
{
    const auto removeAt = [this, &updates](std::vector<Item>::iterator item) {
        updates.push_back({
            ClipboardActivityUpdate::Type::Removed,
            item->header.id,
        });
        return items_.erase(item);
    };

    if (limits_.maxAgeSeconds != 0) {
        const uint64_t maxSupportedAgeSeconds =
            static_cast<uint64_t>((std::chrono::seconds::max)().count());
        if (limits_.maxAgeSeconds <= maxSupportedAgeSeconds) {
            const auto cutoff = now - std::chrono::seconds(limits_.maxAgeSeconds);
            for (auto item = items_.begin(); item != items_.end();) {
                if (item->header.timestamp < cutoff) {
                    item = removeAt(item);
                } else {
                    ++item;
                }
            }
        }
    }

    if (limits_.maxItems != 0) {
        while (static_cast<uint64_t>(items_.size()) > limits_.maxItems) {
            removeAt(items_.begin());
        }
    }

    if (limits_.memoryLimitBytes == 0) {
        return;
    }

    uint64_t totalBytes = 0;
    for (const auto& item : items_) {
        const uint64_t itemBytes = EstimateItemBytes(item);
        if (totalBytes > (std::numeric_limits<uint64_t>::max)() - itemBytes) {
            totalBytes = (std::numeric_limits<uint64_t>::max)();
        } else {
            totalBytes += itemBytes;
        }
    }

    while (items_.size() > 1 && totalBytes > limits_.memoryLimitBytes) {
        const uint64_t itemBytes = EstimateItemBytes(items_.front());
        removeAt(items_.begin());
        totalBytes = itemBytes >= totalBytes ? 0 : totalBytes - itemBytes;
    }
}

std::optional<ClipboardActivityStore::Item> ClipboardActivityStore::FindItem(uint64_t itemID) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(items_.begin(), items_.end(), [itemID](const Item& item) {
        return item.header.id == itemID;
    });

    if (found == items_.end()) {
        return std::nullopt;
    }

    return *found;
}
