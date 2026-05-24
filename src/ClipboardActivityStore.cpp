#include "platform.h"

#include "ClipboardActivityStore.h"
#include "utils.h"

#include <algorithm>
#include <cwctype>
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
    if (payload.formatId != CF_UNICODETEXT) {
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

std::vector<ClipboardActivityItemHeader> ClipboardActivityStore::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SnapshotLocked(items_);
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
    if (!item) {
        return std::nullopt;
    }

    ClipboardPayload payload = item->payload;
    if (!payload.ZstdDecompress()) {
        return std::nullopt;
    }
    return payload;
}

bool ClipboardActivityStore::Remove(uint64_t itemID) {
    ClipboardActivityUpdate update;
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
        update.type = ClipboardActivityUpdate::Type::Removed;
        update.itemID = itemID;
        watchers = watchers_;
    }

    for (const auto& watcher : watchers) {
        if (watcher.watcher) {
            watcher.watcher(update, watcher.userData);
        }
    }

    return true;
}

void ClipboardActivityStore::Clear() {
    ClipboardActivityUpdate update;
    std::vector<WatcherRegistration> watchers;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) {
            return;
        }

        items_.clear();
        update.type = ClipboardActivityUpdate::Type::Cleared;
        watchers = watchers_;
    }

    for (const auto& watcher : watchers) {
        if (watcher.watcher) {
            watcher.watcher(update, watcher.userData);
        }
    }
}

ClipboardActivityRegistration ClipboardActivityStore::QueryAndRegister(Watcher watcher, void* userData) {
    std::lock_guard<std::mutex> lock(mutex_);

    ClipboardActivityRegistration registration;
    registration.watcherID = nextWatcherID_++;
    registration.items = SnapshotLocked(items_);
    watchers_.push_back({ registration.watcherID, std::move(watcher), userData });
    return registration;
}

void ClipboardActivityStore::Unregister(std::size_t watcherID) {
    std::lock_guard<std::mutex> lock(mutex_);
    watchers_.erase(std::remove_if(watchers_.begin(), watchers_.end(), [watcherID](const WatcherRegistration& watcher) {
        return watcher.watcherID == watcherID;
    }), watchers_.end());
}

ClipboardPayload ClipboardActivityStore::MakeStoredPayload(const ClipboardPayload& payload) {
    ClipboardPayload stored = payload;
    if (!stored.isCompressed) {
        stored.ZstdCompress();
    }
    return stored;
}

std::optional<ClipboardActivityDisplayItem> ClipboardActivityStore::BuildDisplayItem(const Item& item) {
    ClipboardPayload payload = item.payload;
    if (!payload.ZstdDecompress()) {
        return std::nullopt;
    }

    ClipboardActivityDisplayItem display;
    display.header = item.header;

    if (payload.formatId == CF_UNICODETEXT) {
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
    } else if (payload.formatId == CF_DIB) {
        display.kind = ClipboardActivityPayloadKind::Image;
        display.imagePngData = std::move(payload.rawData);
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

uint64_t ClipboardActivityStore::AddItem(ClipboardActivityDirection direction, const std::wstring& deviceName, const ClipboardPayload& payload) {
    if (payload.formatId == 0) {
        return 0;
    }

    ClipboardActivityUpdate update;
    std::vector<WatcherRegistration> watchers;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        Item item;
        item.header.id = nextItemID_++;
        item.header.direction = direction;
        item.header.deviceName = deviceName;
        item.header.timestamp = std::chrono::system_clock::now();
        item.payload = MakeStoredPayload(payload);
        item.header.formatId = item.payload.formatId;
        item.header.encodedBytes = item.payload.rawData.size();
        item.header.decodedBytes = item.payload.decodedDataSize;

        update.type = ClipboardActivityUpdate::Type::Added;
        update.itemID = item.header.id;

        items_.push_back(std::move(item));
        watchers = watchers_;
    }

    for (const auto& watcher : watchers) {
        if (watcher.watcher) {
            watcher.watcher(update, watcher.userData);
        }
    }

    return update.itemID;
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
