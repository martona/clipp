#pragma once

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN 
    #define NOMINMAX
    #include <windows.h>
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
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
#elif defined(__APPLE__)
    using PlatformWindowHandle = void*;
    static size_t utf8_to_utf16(const char* utf8, size_t n_utf8, wchar_t* utf16, size_t n_utf16) {
        //TODO
        return 0;
    }

    static size_t utf16_to_utf8(const wchar_t* utf16, size_t n_utf16, char* utf8, size_t n_utf8) {
        //TODO
        return 0;
    }
#endif
