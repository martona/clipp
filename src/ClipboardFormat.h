#pragma once

#include <cstdint>

constexpr uint32_t ClippClipboardFormatCode(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(a)) << 24) |
        (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 8) |
        static_cast<uint32_t>(static_cast<unsigned char>(d));
}

// These are Clipp wire payload formats, not native OS clipboard formats.
// PNG and UTF-8 intentionally keep their original Win32 CF_* numeric values
// so older peers continue to understand them.
inline constexpr uint32_t CLIPP_FORMAT_NONE = 0;
inline constexpr uint32_t CLIPP_FORMAT_PNG = 8;
inline constexpr uint32_t CLIPP_FORMAT_UTF8 = 13;
inline constexpr uint32_t CLIPP_FORMAT_JPEG = ClippClipboardFormatCode('C', 'J', 'P', 'G');

constexpr bool IsClippTextFormat(uint32_t formatId) {
    return formatId == CLIPP_FORMAT_UTF8;
}

constexpr bool IsClippImageFormat(uint32_t formatId) {
    return formatId == CLIPP_FORMAT_PNG || formatId == CLIPP_FORMAT_JPEG;
}

constexpr const char* ClippClipboardFormatName(uint32_t formatId) {
    switch (formatId) {
    case CLIPP_FORMAT_NONE:
        return "none";
    case CLIPP_FORMAT_PNG:
        return "CLIPP_FORMAT_PNG";
    case CLIPP_FORMAT_UTF8:
        return "CLIPP_FORMAT_UTF8";
    case CLIPP_FORMAT_JPEG:
        return "CLIPP_FORMAT_JPEG";
    default:
        return "unknown";
    }
}

constexpr const wchar_t* ClippClipboardFormatNameW(uint32_t formatId) {
    switch (formatId) {
    case CLIPP_FORMAT_NONE:
        return L"none";
    case CLIPP_FORMAT_PNG:
        return L"CLIPP_FORMAT_PNG";
    case CLIPP_FORMAT_UTF8:
        return L"CLIPP_FORMAT_UTF8";
    case CLIPP_FORMAT_JPEG:
        return L"CLIPP_FORMAT_JPEG";
    default:
        return L"unknown";
    }
}
