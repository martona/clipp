#pragma once

#include <cstring>
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <WinSock2.h>
    #include <ws2tcpip.h>
    #include <conio.h>
    // Legacy 16-bit alias (== GetTickCount) nothing here uses. Killed at the
    // source because it poisons C++/WinRT: any winrt impl header processed
    // while it's live bakes "GetTickCount" into Storyboard's declarations, and
    // a later include of Windows.UI.Xaml.Media.Animation.h then fails with
    // "GetCurrentTime is not a member". Per-TU #undefs can't fix that — the
    // first winrt include usually arrives via another header, before them.
    #ifdef GetCurrentTime
    #undef GetCurrentTime
    #endif
#elif defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
    #include <TargetConditionals.h>
    #include <cstddef>
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <cerrno>
#if TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
        // iOS-specific headers live in the platform/ios implementation files.
#elif TARGET_OS_MAC
        // macOS-specific headers will go here eventually
        // e.g., #include <ApplicationServices/ApplicationServices.h>
#else
        #error "Unsupported Apple platform."
#endif
#elif defined(__linux__)
    // Terminal-only Linux build (no GUI, no local clipboard). The POSIX socket
    // surface mirrors the Apple branch; the shared helper block below is guarded
    // with (__APPLE__ || __linux__) and provides the SOCKET shims + UTF codec.
    #include <cstddef>
    #include <cstdio>
    #include <cstdarg>
    #include <cstdlib>
    #include <cwchar>
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <cerrno>
    #include <utf8proc.h>   // Unicode NFC for NormalizeUtf8Canonical (key derivation)
#else
    #error "Unsupported platform!"
#endif


#ifdef _WIN32
    using PlatformWindowHandle = HWND;

    static size_t utf8_to_utf16(const char* utf8, size_t n_utf8, wchar_t* utf16, size_t n_utf16) {
	    return MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n_utf8, utf16, (int)n_utf16);
    }

    static size_t utf16_to_utf8(const wchar_t* utf16, size_t n_utf16, char* utf8, size_t n_utf8) {
	    return WideCharToMultiByte(CP_UTF8, 0, utf16, (int)n_utf16, utf8, (int)n_utf8, nullptr, nullptr);
    }

    static inline int vsnprintf_truncate(char* buffer, size_t size, const char* format, va_list args) {
        return _vsnprintf_s(buffer, size, _TRUNCATE, format, args);
    }

    static inline int vsnwprintf_truncate(wchar_t* buffer, size_t size, const wchar_t* format, va_list args) {
        return _vsnwprintf_s(buffer, size, _TRUNCATE, format, args);
    }

    static inline int localtime_safe(struct tm* tmDest, const time_t* sourceTime) {
        if (!tmDest || !sourceTime) return -1;
        // Windows: Returns errno_t (0 on success)
        return localtime_s(tmDest, sourceTime) == 0 ? 0 : -1;
    }

    typedef int socklen_t;

#elif defined(__APPLE__) || defined(__linux__)
    // Shared POSIX branch (macOS, iOS, Linux). Despite the historical
    // "clipp_apple_encoding_detail" name, nothing here is Apple-specific: the UTF
    // codec is pure C++ (and already handles Linux's 4-byte wchar_t via the
    // `if constexpr (sizeof(wchar_t) == 2)` checks), and the printf/time/socket
    // shims are standard C / POSIX.
    using PlatformWindowHandle = void*;

    namespace clipp_apple_encoding_detail {
        static inline bool DecodeUtf8CodePoint(const char* utf8, size_t n_utf8, size_t& offset, uint32_t& codePoint) {
            if (offset >= n_utf8) {
                return false;
            }

            const auto byteAt = [&](size_t index) -> unsigned char {
                return static_cast<unsigned char>(utf8[index]);
            };

            const size_t start = offset;
            const unsigned char lead = byteAt(offset++);
            if (lead < 0x80) {
                codePoint = lead;
                return true;
            }

            uint32_t value = 0;
            size_t continuationCount = 0;
            uint32_t minimumValue = 0;

            if ((lead & 0xE0) == 0xC0) {
                value = lead & 0x1F;
                continuationCount = 1;
                minimumValue = 0x80;
            } else if ((lead & 0xF0) == 0xE0) {
                value = lead & 0x0F;
                continuationCount = 2;
                minimumValue = 0x800;
            } else if ((lead & 0xF8) == 0xF0) {
                value = lead & 0x07;
                continuationCount = 3;
                minimumValue = 0x10000;
            } else {
                offset = start;
                return false;
            }

            if (offset + continuationCount > n_utf8) {
                offset = start;
                return false;
            }

            for (size_t i = 0; i < continuationCount; ++i) {
                const unsigned char next = byteAt(offset++);
                if ((next & 0xC0) != 0x80) {
                    offset = start;
                    return false;
                }
                value = (value << 6) | (next & 0x3F);
            }

            if (value < minimumValue || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
                offset = start;
                return false;
            }

            codePoint = value;
            return true;
        }

        static inline bool AppendUtf8CodePoint(char* utf8, size_t n_utf8, size_t& offset, uint32_t codePoint) {
            unsigned char bytes[4]{};
            size_t byteCount = 0;

            if (codePoint <= 0x7F) {
                bytes[0] = static_cast<unsigned char>(codePoint);
                byteCount = 1;
            } else if (codePoint <= 0x7FF) {
                bytes[0] = static_cast<unsigned char>(0xC0 | (codePoint >> 6));
                bytes[1] = static_cast<unsigned char>(0x80 | (codePoint & 0x3F));
                byteCount = 2;
            } else if (codePoint <= 0xFFFF) {
                if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
                    return false;
                }
                bytes[0] = static_cast<unsigned char>(0xE0 | (codePoint >> 12));
                bytes[1] = static_cast<unsigned char>(0x80 | ((codePoint >> 6) & 0x3F));
                bytes[2] = static_cast<unsigned char>(0x80 | (codePoint & 0x3F));
                byteCount = 3;
            } else if (codePoint <= 0x10FFFF) {
                bytes[0] = static_cast<unsigned char>(0xF0 | (codePoint >> 18));
                bytes[1] = static_cast<unsigned char>(0x80 | ((codePoint >> 12) & 0x3F));
                bytes[2] = static_cast<unsigned char>(0x80 | ((codePoint >> 6) & 0x3F));
                bytes[3] = static_cast<unsigned char>(0x80 | (codePoint & 0x3F));
                byteCount = 4;
            } else {
                return false;
            }

            if (utf8 != nullptr && offset + byteCount <= n_utf8) {
                std::memcpy(utf8 + offset, bytes, byteCount);
            }
            offset += byteCount;
            return true;
        }
    }

    static inline size_t utf8_to_utf16(const char* utf8, size_t n_utf8, wchar_t* utf16, size_t n_utf16) {
        if (!utf8) return 0;

        size_t inputOffset = 0;
        size_t outputOffset = 0;
        while (inputOffset < n_utf8) {
            uint32_t codePoint = 0;
            if (!clipp_apple_encoding_detail::DecodeUtf8CodePoint(utf8, n_utf8, inputOffset, codePoint)) {
                return 0;
            }

            if constexpr (sizeof(wchar_t) == 2) {
                if (codePoint <= 0xFFFF) {
                    if (utf16 != nullptr && outputOffset < n_utf16) {
                        utf16[outputOffset] = static_cast<wchar_t>(codePoint);
                    }
                    ++outputOffset;
                } else {
                    const uint32_t shifted = codePoint - 0x10000;
                    if (utf16 != nullptr && outputOffset < n_utf16) {
                        utf16[outputOffset] = static_cast<wchar_t>(0xD800 | (shifted >> 10));
                    }
                    ++outputOffset;
                    if (utf16 != nullptr && outputOffset < n_utf16) {
                        utf16[outputOffset] = static_cast<wchar_t>(0xDC00 | (shifted & 0x3FF));
                    }
                    ++outputOffset;
                }
            } else {
                if (utf16 != nullptr && outputOffset < n_utf16) {
                    utf16[outputOffset] = static_cast<wchar_t>(codePoint);
                }
                ++outputOffset;
            }
        }

        return utf16 == nullptr || n_utf16 == 0 ? outputOffset : (std::min)(outputOffset, n_utf16);
    }

    static inline size_t utf16_to_utf8(const wchar_t* utf16, size_t n_utf16, char* utf8, size_t n_utf8) {
        if (!utf16) return 0;

        size_t outputOffset = 0;
        for (size_t i = 0; i < n_utf16; ++i) {
            uint32_t codePoint = static_cast<uint32_t>(utf16[i]);
            if constexpr (sizeof(wchar_t) == 2) {
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (i + 1 >= n_utf16) {
                        return 0;
                    }

                    const uint32_t low = static_cast<uint32_t>(utf16[++i]);
                    if (low < 0xDC00 || low > 0xDFFF) {
                        return 0;
                    }

                    codePoint = 0x10000 + (((codePoint - 0xD800) << 10) | (low - 0xDC00));
                } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                    return 0;
                }
            }

            if (!clipp_apple_encoding_detail::AppendUtf8CodePoint(utf8, n_utf8, outputOffset, codePoint)) {
                return 0;
            }
        }

        return utf8 == nullptr || n_utf8 == 0 ? outputOffset : (std::min)(outputOffset, n_utf8);
    }

    static inline int vsnprintf_truncate(char* buffer, size_t size, const char* format, va_list args) {
        return std::vsnprintf(buffer, size, format, args);
    }

    static inline int vsnwprintf_truncate(wchar_t* buffer, size_t size, const wchar_t* format, va_list args) {
        return std::vswprintf(buffer, size, format, args);
    }

    static inline int localtime_safe(struct tm* tmDest, const time_t* sourceTime) {
        if (!tmDest || !sourceTime) return -1;
        // macOS / POSIX: Returns pointer to tm (nullptr on failure)
        return localtime_r(sourceTime, tmDest) != nullptr ? 0 : -1;
    }

    typedef int SOCKET;
    const int INVALID_SOCKET = -1;
    const int SOCKET_ERROR = -1;
    #define SD_RECEIVE SHUT_RD
    #define SD_SEND    SHUT_WR
    #define SD_BOTH    SHUT_RDWR
    static inline void closesocket(SOCKET s) { close(s); }
#endif

namespace clipp_platform_detail {
    static inline bool DecodeUtf8CodePoint(std::string_view text, std::size_t& offset, uint32_t& codePoint) {
        if (offset >= text.size()) {
            return false;
        }

        const auto byteAt = [&](std::size_t index) -> unsigned char {
            return static_cast<unsigned char>(text[index]);
        };

        const std::size_t start = offset;
        const unsigned char lead = byteAt(offset++);
        if (lead < 0x80) {
            codePoint = lead;
            return true;
        }

        uint32_t value = 0;
        std::size_t continuationCount = 0;
        uint32_t minimumValue = 0;

        if ((lead & 0xE0) == 0xC0) {
            value = lead & 0x1F;
            continuationCount = 1;
            minimumValue = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            value = lead & 0x0F;
            continuationCount = 2;
            minimumValue = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            value = lead & 0x07;
            continuationCount = 3;
            minimumValue = 0x10000;
        } else {
            codePoint = lead;
            return false;
        }

        if (offset + continuationCount > text.size()) {
            offset = start + 1;
            codePoint = lead;
            return false;
        }

        for (std::size_t i = 0; i < continuationCount; ++i) {
            const unsigned char next = byteAt(offset++);
            if ((next & 0xC0) != 0x80) {
                offset = start + 1;
                codePoint = lead;
                return false;
            }
            value = (value << 6) | (next & 0x3F);
        }

        if (value < minimumValue || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            offset = start + 1;
            codePoint = lead;
            return false;
        }

        codePoint = value;
        return true;
    }

    static inline void AppendUtf8CodePoint(std::string& output, uint32_t codePoint) {
        if (codePoint <= 0x7F) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    static inline bool FoldKeyDerivationCodePoint(uint32_t codePoint, char& folded) {
        switch (codePoint) {
        case 0x0060: // grave accent
        case 0x00B4: // acute accent
        case 0x02BC: // modifier letter apostrophe
        case 0x2018: // left single quotation mark
        case 0x2019: // right single quotation mark
        case 0x201A: // single low-9 quotation mark
        case 0x201B: // single high-reversed-9 quotation mark
        case 0x2032: // prime
        case 0x2035: // reversed prime
        case 0xFF07: // fullwidth apostrophe
            folded = '\'';
            return true;
        case 0x201C: // left double quotation mark
        case 0x201D: // right double quotation mark
        case 0x201E: // double low-9 quotation mark
        case 0x201F: // double high-reversed-9 quotation mark
        case 0x2033: // double prime
        case 0x2036: // reversed double prime
        case 0xFF02: // fullwidth quotation mark
            folded = '"';
            return true;
        case 0x00A0: // no-break space
        case 0x1680:
        case 0x202F: // narrow no-break space
        case 0x205F:
        case 0x3000:
            folded = ' ';
            return true;
        case 0x2010:
        case 0x2011:
        case 0x2012:
        case 0x2013:
        case 0x2014:
        case 0x2015:
        case 0x2212:
        case 0xFE58:
        case 0xFE63:
        case 0xFF0D:
            folded = '-';
            return true;
        default:
            if (codePoint >= 0x2000 && codePoint <= 0x200A) {
                folded = ' ';
                return true;
            }
            return false;
        }
    }

    static inline std::string FoldKeyDerivationTextVariants(std::string_view text) {
        std::string output;
        output.reserve(text.size());

        std::size_t offset = 0;
        while (offset < text.size()) {
            const std::size_t start = offset;
            uint32_t codePoint = 0;
            if (!DecodeUtf8CodePoint(text, offset, codePoint)) {
                output.push_back(text[start]);
                continue;
            }

            char folded = '\0';
            if (FoldKeyDerivationCodePoint(codePoint, folded)) {
                output.push_back(folded);
            } else {
                AppendUtf8CodePoint(output, codePoint);
            }
        }

        return output;
    }

    static inline std::string Utf16ToUtf8String(const std::wstring& value) {
        if (value.empty()) {
            return {};
        }

        const size_t size = utf16_to_utf8(value.c_str(), value.size(), nullptr, 0);
        if (size == 0) {
            return {};
        }

        std::string result(size, '\0');
        utf16_to_utf8(value.c_str(), value.size(), result.data(), result.size());
        return result;
    }

    static inline std::wstring Utf8ToUtf16String(std::string_view value) {
        if (value.empty()) {
            return {};
        }

        const size_t size = utf8_to_utf16(value.data(), value.size(), nullptr, 0);
        if (size == 0) {
            return {};
        }

        std::wstring result(size, L'\0');
        utf8_to_utf16(value.data(), value.size(), result.data(), result.size());
        return result;
    }

    static inline std::string NormalizeUtf8Canonical(std::string_view value) {
        if (value.empty()) {
            return {};
        }

#ifdef _WIN32
        std::wstring wide = Utf8ToUtf16String(value);
        if (wide.empty()) {
            return std::string(value);
        }

        const int needed = NormalizeString(
            NormalizationC,
            wide.data(),
            static_cast<int>(wide.size()),
            nullptr,
            0);
        if (needed <= 0) {
            return std::string(value);
        }

        std::wstring normalized(static_cast<std::size_t>(needed), L'\0');
        const int written = NormalizeString(
            NormalizationC,
            wide.data(),
            static_cast<int>(wide.size()),
            normalized.data(),
            needed);
        if (written <= 0) {
            return std::string(value);
        }

        normalized.resize(static_cast<std::size_t>(written));
        return Utf16ToUtf8String(normalized);
#elif defined(__APPLE__)
        CFStringRef source = CFStringCreateWithBytes(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(value.data()),
            static_cast<CFIndex>(value.size()),
            kCFStringEncodingUTF8,
            false);
        if (source == nullptr) {
            return std::string(value);
        }

        CFMutableStringRef normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source);
        CFRelease(source);
        if (normalized == nullptr) {
            return std::string(value);
        }

        CFStringNormalize(normalized, kCFStringNormalizationFormC);
        const CFIndex length = CFStringGetLength(normalized);
        const CFIndex maxUtf8Bytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
        if (maxUtf8Bytes < 0) {
            CFRelease(normalized);
            return std::string(value);
        }

        std::vector<char> buffer(static_cast<std::size_t>(maxUtf8Bytes) + 1, '\0');
        if (!CFStringGetCString(normalized, buffer.data(), static_cast<CFIndex>(buffer.size()), kCFStringEncodingUTF8)) {
            CFRelease(normalized);
            return std::string(value);
        }

        CFRelease(normalized);
        return std::string(buffer.data());
#elif defined(__linux__)
        // utf8proc NFC. Must agree byte-for-byte with Windows
        // NormalizeString(NormalizationC) and macOS kCFStringNormalizationFormC --
        // otherwise a decomposed (NFD) non-ASCII network name/password derives a
        // different key here than on the other platforms and the devices silently
        // fail to join the same network. (All three implement Unicode NFC, so they
        // should agree; verify with an accented + a CJK sample at first build.)
        utf8proc_uint8_t* normalized = nullptr;
        const utf8proc_ssize_t length = utf8proc_map(
            reinterpret_cast<const utf8proc_uint8_t*>(value.data()),
            static_cast<utf8proc_ssize_t>(value.size()),
            &normalized,
            static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
        if (length < 0 || normalized == nullptr) {
            return std::string(value);  // normalization failed: fall back to raw input
        }
        std::string result(reinterpret_cast<const char*>(normalized), static_cast<size_t>(length));
        free(normalized);
        return result;
#else
        return std::string(value);
#endif
    }
}

static inline std::string CanonicalizeKeyDerivationText(std::string_view value) {
    return clipp_platform_detail::FoldKeyDerivationTextVariants(
        clipp_platform_detail::NormalizeUtf8Canonical(value));
}

static inline void strncpys(char* dst, const char* src, size_t maxlen) {
    if (!dst || !src || maxlen == 0) return;
	// option 1: two iterations (find length, then copy)
    //size_t copyLen = (std::min)(std::strlen(src), maxlen - 1);
    //std::memcpy(dst, src, copyLen);
    //dst[copyLen] = '\0';
    // option 2: single iteration but zeroes out the entire destination buffer
    //std::strncpy(dst, src, maxlen - 1);
    //dst[maxlen - 1] = '\0';
	// option 3: least bad
    #ifdef _MSC_VER
        strncpy_s(dst, maxlen, src, _TRUNCATE);
    #elif defined(__APPLE__)
        strlcpy(dst, src, maxlen);
    #else
        dst[0] = '\0';
        std::strncat(dst, src, maxlen - 1);
    #endif
}

static inline int snwprintf_truncate(wchar_t* buffer, size_t size, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnwprintf_truncate(buffer, size, format, args);
    va_end(args);
    return result;
}

template <size_t N>
static inline void strncpys(char(&dst)[N], const char* src) {
    strncpys(dst, src, N);
}

#define cntof(arr) (sizeof(arr) / sizeof(arr[0]))

enum class SingleInstanceResult {
    Continue,
    ExitSuccess,
    ExitFailure,
};

SingleInstanceResult EnsureSingleInstance();
void StopSingleInstanceServer();
bool RegisterClippAutoStart();
bool UnregisterClippAutoStart();

#if defined(__APPLE__)
// True when running as the Mac App Store flavor. There is no compile-time MAS
// flag (build_macos_mas.sh builds the identical binary and only signs it
// differently), but MAS is the only flavor signed with the app-sandbox
// entitlement, so detecting the sandbox at runtime is equivalent.
bool IsMacAppStoreBuild();
// Login-item state for the MAS consent toggle (guideline 2.4.5(iii)): the MAS
// flavor must not register a login item without explicit consent, so its
// Settings page drives state through these instead of the
// register-on-start/unregister-on-exit default.
bool IsClippAutoStartEnabled();
void OpenClippLoginItemsSettings();
void RequestMacOSShowMainWindow(bool showNetworkPage = false);
void RunMacOSStatusMenu(bool showNetworkPageOnStartup = false);
void RequestMacOSAppShutdown(bool unregisterAutoStart = false);
#else
inline bool IsMacAppStoreBuild() { return false; }
#endif

#ifndef NDEBUG
    #ifdef _WIN32
        #define CLIPP_DEBUG_BREAK() __debugbreak()
    #elif defined(__APPLE__) || defined(__linux__)
        #if __has_builtin(__builtin_debugtrap)
            #define CLIPP_DEBUG_BREAK() __builtin_debugtrap()
        #else
            #define CLIPP_DEBUG_BREAK() __builtin_trap()
        #endif
    #else
        #include <cstdlib>
        #define CLIPP_DEBUG_BREAK() std::abort()
    #endif
    #define CLIPP_ASSERT(condition) \
            do { \
                if (!(condition)) { \
                    CLIPP_DEBUG_BREAK(); \
                } \
            } while (false)
#else
#define CLIPP_ASSERT(condition) do { (void)sizeof(condition); } while(false)
#define CLIPP_DEBUG_BREAK() do {} while(0)
#endif
