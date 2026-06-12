#include "platform.h"

#include "ClipboardActivityStore.h"
#include "Settings.h"
#include "platform/uistrings.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace {
constexpr std::size_t kMaxTextPreviewCharacters = 640;

std::wstring TrimCopy(const std::wstring& text) {
    std::size_t first = 0;
    while (first < text.size() && std::iswspace(static_cast<wint_t>(text[first])) != 0) {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first && std::iswspace(static_cast<wint_t>(text[last - 1])) != 0) {
        --last;
    }

    return text.substr(first, last - first);
}

bool StartsWithInsensitive(const std::wstring& text, const wchar_t* prefix) {
    const std::wstring_view prefixView(prefix);
    if (text.size() < prefixView.size()) {
        return false;
    }

    for (std::size_t i = 0; i < prefixView.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(text[i])) !=
            std::towlower(static_cast<wint_t>(prefixView[i]))) {
            return false;
        }
    }

    return true;
}

bool HasWhitespace(const std::wstring& text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) {
        return std::iswspace(static_cast<wint_t>(ch)) != 0;
    });
}

bool LooksPrivateText(const std::wstring& text) {
    return !text.empty() && text.size() <= 256 && !HasWhitespace(text);
}

bool LooksLikeUrl(const std::wstring& text) {
    if (!(StartsWithInsensitive(text, L"http://") || StartsWithInsensitive(text, L"https://"))) {
        return false;
    }

    return !HasWhitespace(text);
}

std::wstring ExtractUrlHost(const std::wstring& url) {
    const std::size_t schemeEnd = url.find(L"://");
    if (schemeEnd == std::wstring::npos) {
        return {};
    }

    const std::size_t hostStart = schemeEnd + 3;
    std::size_t hostEnd = url.find_first_of(L"/?#", hostStart);
    if (hostEnd == std::wstring::npos) {
        hostEnd = url.size();
    }

    std::wstring host = url.substr(hostStart, hostEnd - hostStart);
    const std::size_t userInfoEnd = host.rfind(L'@');
    if (userInfoEnd != std::wstring::npos) {
        host.erase(0, userInfoEnd + 1);
    }
    return host;
}

std::wstring PreviewText(const std::wstring& text) {
    if (text.size() <= kMaxTextPreviewCharacters) {
        return text;
    }

    std::wstring preview = text.substr(0, kMaxTextPreviewCharacters);
    preview += L"...";
    return preview;
}

std::optional<std::wstring> TextFromPayload(const ClipboardPayload& payload) {
    if (payload.meta.formatId != CLIPP_FORMAT_UTF8) {
        return std::nullopt;
    }

    const std::vector<unsigned char>* bytes = payload.TryGetUncompressedBytes();
    if (bytes == nullptr) {
        return std::nullopt;
    }

    std::string textUtf8(bytes->begin(), bytes->end());
    while (!textUtf8.empty() && textUtf8.back() == '\0') {
        textUtf8.pop_back();
    }

    return Utf8ToWideString(textUtf8);
}
}

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

std::optional<ClipboardActivityDisplayItem> ClipboardActivityStore::DisplayItem(uint64_t itemID) const {
    const auto item = FindItem(itemID);
    if (!item) {
        return std::nullopt;
    }

    return BuildDisplayItem(*item);
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

std::optional<ClipboardActivityDisplayItem> ClipboardActivityStore::BuildDisplayItem(const Item& item) {
    if (!item.payload) {
        return std::nullopt;
    }

    ClipboardActivityDisplayItem display;
    display.header = item.header;

    HostId localHostId;
    g_settings.getHostID(localHostId);
    const HostId originHostId(item.payload->meta.originHostId);
    if (originHostId == localHostId) {
        display.direction = ClipboardActivityDirection::Outgoing;
        display.deviceName = CLP_W(CLP_UI_THIS_DEVICE);
    } else {
        display.direction = ClipboardActivityDirection::Incoming;
        display.deviceName = Utf8ToWideString(item.payload->meta.originHostName);
    }

    const bool sourceMarkedPrivate =
        (item.payload->meta.flags & NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE) != 0;

    // Source-marked-private + empty payload = sender's "sync skipped" placeholder.
    // Render as an information-only entry; the UI suppresses copy-back.
    if (sourceMarkedPrivate && item.payload->EncodedBytes().empty()) {
        display.kind = ClipboardActivityPayloadKind::PrivatePlaceholder;
        display.sourceMarked = true;
        return display;
    }

    if (IsClippImageFormat(item.payload->meta.formatId)) {
        // Image payloads aren't zstd-compressed, so EncodedBytes() IS the image —
        // expose it via an aliasing shared_ptr so the UI shares the buffer
        // without copying. The aliasing keeps the underlying payload alive.
        display.kind = ClipboardActivityPayloadKind::Image;
        display.imageFormatId = item.payload->meta.formatId;
        display.imageData = std::shared_ptr<const std::vector<unsigned char>>(
            item.payload, &item.payload->EncodedBytes());
        return display;
    }

    if (item.payload->meta.formatId == CLIPP_FORMAT_UTF8) {
        auto text = TextFromPayload(*item.payload);
        if (!text) {
            return std::nullopt;
        }

        display.detailText = *text;
        const std::wstring trimmed = TrimCopy(*text);
        if (sourceMarkedPrivate) {
            // Explicit privacy signal from the source app overrides the heuristic
            // classification: mask, and record that the marker (not the heuristic)
            // is responsible so the UI can attach a "private" badge.
            display.kind = ClipboardActivityPayloadKind::PrivateText;
            display.sourceMarked = true;
            display.previewText = L"••••••••";
            display.revealedPreviewText = PreviewText(*text);
        } else if (LooksLikeUrl(trimmed)) {
            display.kind = ClipboardActivityPayloadKind::Link;
            display.previewText = PreviewText(trimmed);
            display.linkHost = ExtractUrlHost(trimmed);
        } else if (LooksPrivateText(trimmed)) {
            display.kind = ClipboardActivityPayloadKind::PrivateText;
            display.previewText = L"••••••••";
            display.revealedPreviewText = PreviewText(*text);
        } else {
            display.kind = ClipboardActivityPayloadKind::Text;
            display.previewText = PreviewText(*text);
        }
    } else {
        display.kind = ClipboardActivityPayloadKind::Unsupported;
    }

    return display;
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
        const auto& guid = payload->meta.eventGuid;
        const bool guidIsZero = std::all_of(std::begin(guid), std::end(guid),
            [](uint8_t b) { return b == 0; });
        if (!guidIsZero) {
            for (const auto& existing : items_) {
                if (existing.payload &&
                    std::memcmp(existing.payload->meta.eventGuid, guid, sizeof(guid)) == 0) {
                    return existing.header.id;
                }
            }
        }

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
