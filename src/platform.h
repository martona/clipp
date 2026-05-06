#pragma once

#include <cstring>
#include <algorithm>
#include <ctime>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN 
    #define NOMINMAX
    #include <windows.h>
    #include <WinSock2.h>
    #include <ws2tcpip.h>
    #include <conio.h>
#elif defined(__APPLE__)
    #define _LIBCPP_DISABLE_DEPRECATION_WARNINGS 1
    #include <TargetConditionals.h>
    #include <cstddef>
    #include <codecvt>
    #include <locale>
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <cerrno>
#if TARGET_OS_MAC
        // macOS-specific headers will go here eventually
        // e.g., #include <ApplicationServices/ApplicationServices.h>
    #else
        #error "iOS is not currently supported."
    #endif
#elif defined(__linux__)
    #error "Not currently supported."
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

#elif defined(__APPLE__)
    using PlatformWindowHandle = void*;

    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"

    static size_t utf8_to_utf16(const char* utf8, size_t n_utf8, wchar_t* utf16, size_t n_utf16) {
        if (!utf8) return 0;

        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        std::wstring wide;
        try {
            wide = converter.from_bytes(utf8, utf8 + n_utf8);
        } catch (...) {
            return 0;
        }

        if (utf16 == nullptr || n_utf16 == 0) {
            return wide.size();
        }

        const size_t copyLen = (std::min)(wide.size(), n_utf16);
        std::memcpy(utf16, wide.data(), copyLen * sizeof(wchar_t));
        return copyLen;
    }

    static size_t utf16_to_utf8(const wchar_t* utf16, size_t n_utf16, char* utf8, size_t n_utf8) {
        if (!utf16) return 0;

        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        std::string narrow;
        try {
            narrow = converter.to_bytes(utf16, utf16 + n_utf16);
        } catch (...) {
            return 0;
        }

        if (utf8 == nullptr || n_utf8 == 0) {
            return narrow.size();
        }

        const size_t copyLen = (std::min)(narrow.size(), n_utf8);
        std::memcpy(utf8, narrow.data(), copyLen);
        return copyLen;
    }

    #pragma clang diagnostic pop

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
