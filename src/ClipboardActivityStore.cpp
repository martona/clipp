#include "platform.h"

#include "ClipboardActivityStore.h"
#include "utils.h"

#include <algorithm>
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
    return text.size() >= 8 && text.size() <= 256 && !HasWhitespace(text);
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
    if (payload.formatId != CLIPP_FORMAT_UTF8) {
        return std::nullopt;
    }

    std::string textUtf8(payload.rawData.begin(), payload.rawData.end());
    while (!textUtf8.empty() && textUtf8.back() == '\0') {
        textUtf8.pop_back();
    }

    return Utf8ToWideString(textUtf8);
}
}

uint64_t ClipboardActivityStore::AddIncoming(const std::wstring& deviceName, const ClipboardPayload& payload) {
    return AddItem(ClipboardActivityDirection::Incoming, deviceName, payload);
}

uint64_t ClipboardActivityStore::AddOutgoing(const std::wstring& deviceName, const ClipboardPayload& payload) {
    return AddItem(ClipboardActivityDirection::Outgoing, deviceName, payload);
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

std::optional<ClipboardPayload> ClipboardActivityStore::PayloadForClipboard(uint64_t itemID) const {
    const auto item = FindItem(itemID);
    if (!item || !item->payload) {
        return std::nullopt;
    }

    ClipboardPayload payload = *item->payload;
    if (!payload.ZstdDecompress()) {
        return std::nullopt;
    }
    return payload;
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

std::shared_ptr<const ClipboardPayload> ClipboardActivityStore::MakeStoredPayload(const ClipboardPayload& payload) {
    ClipboardPayload stored = payload;
    if (!stored.isCompressed) {
        stored.ZstdCompress();
    }
    return std::make_shared<const ClipboardPayload>(std::move(stored));
}

std::optional<ClipboardActivityDisplayItem> ClipboardActivityStore::BuildDisplayItem(const Item& item) {
    if (!item.payload) {
        return std::nullopt;
    }

    ClipboardPayload payload = *item.payload;
    if (!payload.ZstdDecompress()) {
        return std::nullopt;
    }

    ClipboardActivityDisplayItem display;
    display.header = item.header;

    if (payload.formatId == CLIPP_FORMAT_UTF8) {
        auto text = TextFromPayload(payload);
        if (!text) {
            return std::nullopt;
        }

        display.detailText = *text;
        const std::wstring trimmed = TrimCopy(*text);
        if (LooksLikeUrl(trimmed)) {
            display.kind = ClipboardActivityPayloadKind::Link;
            display.previewText = PreviewText(trimmed);
            display.linkHost = ExtractUrlHost(trimmed);
        } else if (LooksPrivateText(trimmed)) {
            display.kind = ClipboardActivityPayloadKind::PrivateText;
            display.previewText = L"••••••••";
        } else {
            display.kind = ClipboardActivityPayloadKind::Text;
            display.previewText = PreviewText(*text);
        }
    } else if (IsClippImageFormat(payload.formatId)) {
        display.kind = ClipboardActivityPayloadKind::Image;
        display.imageFormatId = payload.formatId;
        display.imageData = std::move(payload.rawData);
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
        ? static_cast<uint64_t>(item.payload->rawData.size())
        : 0;
    const uint64_t deviceNameBytes = static_cast<uint64_t>(item.header.deviceName.size() * sizeof(wchar_t));
    return kMetadataEstimateBytes + payloadBytes + deviceNameBytes;
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

uint64_t ClipboardActivityStore::AddItem(ClipboardActivityDirection direction, const std::wstring& deviceName, const ClipboardPayload& payload) {
    if (payload.formatId == CLIPP_FORMAT_NONE) {
        return 0;
    }

    uint64_t itemID = 0;
    std::vector<ClipboardActivityUpdate> updates;
    std::vector<WatcherRegistration> watchers;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        Item item;
        item.header.id = nextItemID_++;
        item.header.direction = direction;
        item.header.deviceName = deviceName;
        item.header.timestamp = std::chrono::system_clock::now();
        item.payload = MakeStoredPayload(payload);
        item.header.formatId = item.payload->formatId;
        item.header.encodedBytes = item.payload->rawData.size();
        item.header.uncompressedBytes = item.payload->uncompressedDataSize;

        itemID = item.header.id;
        updates.push_back({
            ClipboardActivityUpdate::Type::Added,
            itemID,
        });

        items_.push_back(std::move(item));
        ApplyLimitsLocked(std::chrono::system_clock::now(), updates);
        watchers = watchers_;
    }

    NotifyWatchers(watchers, updates);

    return itemID;
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
