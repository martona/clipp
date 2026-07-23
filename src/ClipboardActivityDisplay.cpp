#include "platform.h"

#include "ClipboardActivityStore.h"
#include "Settings.h"
#include "platform/uistrings.h"
#include "utils.h"

#include <algorithm>
#include <cwctype>
#include <optional>
#include <string_view>

// Display-item construction for ClipboardActivityStore. Split from the store's
// container logic (ClipboardActivityStore.cpp) because this half depends on
// g_settings / uistrings / text heuristics, while the container half is pure
// engine code the unit tests compile standalone.

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

std::optional<ClipboardActivityDisplayItem> ClipboardActivityStore::DisplayItem(uint64_t itemID) const {
    const auto item = FindItem(itemID);
    if (!item) {
        return std::nullopt;
    }

    return BuildDisplayItem(*item);
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
        } else if (g_settings.maskShortTextPreviews() && LooksPrivateText(trimmed)) {
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
