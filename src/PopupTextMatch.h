#pragma once

// Shared type-to-find text logic for the visual-paste popup shells (win32
// XAML island + macOS AppKit panel). Pure std::wstring in/out — no platform
// types — so both shells run the identical matching, re-windowing, and
// fits-in-a-row rules. Match visibility is BY CONSTRUCTION everywhere: rows
// re-window their line around the first hit and the flyout shows the region
// around it; no scrolling APIs are involved (they proved unreliable).

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace popupfind {

// XAML/AppKit row construction is the expensive part of a re-render; cap what
// one filter state shows (per column) and say so with a hint row.
inline constexpr std::size_t kMaxRenderedRows = 40;
// Row text is one ellipsized line; text this short fits it and earns no flyout.
inline constexpr std::size_t kRowFitChars = 60;
// Context kept ahead of the first match when a row re-windows its line.
inline constexpr std::size_t kRowMatchLeadChars = 12;
// The flyout's window: how much context precedes the first match, and how much
// total text is shown.
inline constexpr std::size_t kPreviewLeadChars = 400;
inline constexpr std::size_t kPreviewWindowChars = 2500;
// A one-letter filter over a big text can hit thousands of times; highlighters
// cap out rather than drowning the renderer.
inline constexpr std::size_t kMaxHighlightRanges = 200;
// Register content previews mirror the history display's preview cap: find
// matches against this window, the flyout against the full value.
inline constexpr std::size_t kRegisterPreviewChars = 640;

// The same ASCII folding PopupModel's filter uses, so what matched is what
// lights up. Non-ASCII compares exact.
inline wchar_t FoldAscii(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
}

// Non-overlapping, ASCII-case-insensitive occurrences of `needle`, capped at
// kMaxHighlightRanges.
inline std::vector<std::size_t> FindMatches(const std::wstring& text, const std::wstring& needle) {
    std::vector<std::size_t> matches;
    if (needle.empty() || needle.size() > text.size()) {
        return matches;
    }
    for (std::size_t start = 0; start + needle.size() <= text.size(); ++start) {
        bool hit = true;
        for (std::size_t i = 0; i < needle.size(); ++i) {
            if (FoldAscii(text[start + i]) != FoldAscii(needle[i])) {
                hit = false;
                break;
            }
        }
        if (hit) {
            matches.push_back(start);
            if (matches.size() >= kMaxHighlightRanges) {
                break;
            }
            start += needle.size() - 1;
        }
    }
    return matches;
}

// Trailing shell newlines (a piped `clipp copy`) must not force a flyout for
// one short line: the fits-in-a-row decision looks at the whitespace-trimmed
// core. The flyout, when it does earn its keep, shows the value untrimmed.
inline bool TextFitsInRow(const std::wstring& full) {
    const auto first = full.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return true;  // nothing but whitespace: nothing a flyout could add
    }
    const auto last = full.find_last_not_of(L" \t\r\n");
    const std::wstring_view core(full.data() + first, last - first + 1);
    return core.find(L'\n') == std::wstring_view::npos && core.size() <= kRowFitChars;
}

// Row line re-window: a content match past the single-line ellipsis would be
// invisible, so shift the line to start shortly before the first match.
inline std::wstring ReWindowRowText(std::wstring text, const std::wstring& filter) {
    if (filter.empty()) {
        return text;
    }
    const auto matches = FindMatches(text, filter);
    if (!matches.empty() && matches.front() > kRowMatchLeadChars) {
        text = L"…" + text.substr(matches.front() - kRowMatchLeadChars);
    }
    return text;
}

// Flyout body: the REGION around the first filter match, with clip markers on
// whichever ends were cut.
inline std::wstring WindowAroundFirstMatch(const std::wstring& full, const std::wstring& filter) {
    std::size_t firstMatch = std::wstring::npos;
    if (!filter.empty()) {
        const auto matches = FindMatches(full, filter);
        if (!matches.empty()) {
            firstMatch = matches.front();
        }
    }
    std::size_t begin = 0;
    if (firstMatch != std::wstring::npos && firstMatch > kPreviewLeadChars) {
        begin = firstMatch - kPreviewLeadChars;
    }
    std::wstring shown = full.substr(begin, kPreviewWindowChars);
    const bool clippedFront = begin > 0;
    const bool clippedBack = begin + shown.size() < full.size();
    if (clippedFront) {
        shown.insert(0, L"… ");
    }
    if (clippedBack) {
        shown.append(L" …");
    }
    return shown;
}

}  // namespace popupfind
