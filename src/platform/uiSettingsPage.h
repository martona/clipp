#pragma once

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>

namespace uiSettingsPage {
inline std::string TrimAscii(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

inline bool TryParsePort(std::string_view text, int& port) {
    const std::string value = TrimAscii(text);
    if (value.empty() || value.size() > 5) {
        return false;
    }

    int parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < 1 || parsed > 65535) {
        return false;
    }

    port = parsed;
    return true;
}
}
