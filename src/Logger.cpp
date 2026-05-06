#include "Logger.h"

#include "platform.h"

#include <chrono>
#include <cstdarg>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger g_logger;

void Logger::log(const wchar_t* function, Level level, const wchar_t* message, ...) {
    va_list args;
    va_start(args, message);
    logV(function, level, message, args);
    va_end(args);
}

void Logger::log(const char* function, Level level, const char* message, ...) {
    va_list args;
    va_start(args, message);
    logV(function, level, message, args);
    va_end(args);
}

void Logger::log(const char* function, Level level, const wchar_t* message, ...) {
    std::wstring functionW = Utf8ToWide(function != nullptr ? function : "");
    va_list args;
    va_start(args, message);
    logV(functionW.c_str(), level, message, args);
    va_end(args);
}

void Logger::log(const wchar_t* function, Level level, const char* message, ...) {
    va_list args;
    va_start(args, message);
    constexpr size_t kBufferSize = 4096;
    char buffer[kBufferSize]{};
    vsnprintf_truncate(buffer, kBufferSize, message != nullptr ? message : "", args);
    va_end(args);

    const std::wstring messageW = Utf8ToWide(buffer);
    writeLine(function, level, messageW.c_str());
}

void Logger::logV(const char* function, Level level, const char* message, va_list args) {
    constexpr size_t kBufferSize = 4096;
    char buffer[kBufferSize]{};
    vsnprintf_truncate(buffer, kBufferSize, message != nullptr ? message : "", args);

    const std::wstring functionW = Utf8ToWide(function != nullptr ? function : "");
    const std::wstring messageW = Utf8ToWide(buffer);
    writeLine(functionW.c_str(), level, messageW.c_str());
}

void Logger::logV(const wchar_t* function, Level level, const wchar_t* message, va_list args) {
    constexpr size_t kBufferSize = 4096;
    wchar_t buffer[kBufferSize]{};
    vsnwprintf_truncate(buffer, kBufferSize, message != nullptr ? message : L"", args);
    writeLine(function, level, buffer);
}

void Logger::writeLine(const wchar_t* function, Level level, const wchar_t* message) {
    auto now = std::chrono::system_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t nowTime = std::chrono::system_clock::to_time_t(now);

    std::tm tmNow{};
    localtime_safe(&tmNow, &nowTime);

    std::wostringstream timestamp;
    timestamp << std::put_time(&tmNow, L"%Y-%m-%d %H:%M:%S") << L'.' << std::setfill(L'0') << std::setw(3) << millis.count();

    std::lock_guard<std::mutex> lock(mutex_);
    std::wcout << L"\x1b[90m" << timestamp.str()
        << LevelToColor(level) << L" [" << LevelToString(level) << L"] "
        << L"\x1b[90m" << "[" << (function != nullptr ? function : L"") << L"] "
        << ResetColor() << (message != nullptr ? message : L"<null>") << std::endl;
}

const wchar_t* Logger::LevelToString(Level level) {
    switch (level) {
    case Level::Debug:
        return L"debug";
    case Level::Info:
        return L"info";
    case Level::Warning:
        return L"warning";
    case Level::Error:
        return L"error";
    default:
        return L"unknown";
    }
}

const wchar_t* Logger::LevelToColor(Level level) {
    // \x1b[ is the escape sequence. 
    // The trailing 'm' signals the end of the color code.
    switch (level) {
    case Level::Debug:
        return L"\x1b[36m";       // Cyan (Clear, but visually recedes slightly for spammy logs)
    case Level::Info:
        return L"\x1b[1;32m";     // Bold Green (Bright, positive confirmation)
    case Level::Warning:
        return L"\x1b[1;33m";     // Bold Yellow (High contrast, catches the eye)
    case Level::Error:
        return L"\x1b[1;31m";     // Bold Red (Maximum urgency)
    default:
        return L"\x1b[0m";        // Reset (Returns to default terminal foreground)
    }
}

const wchar_t* Logger::ResetColor() {
    return L"\x1b[0m";
}

std::wstring Logger::Utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";

	size_t size = utf8_to_utf16(value.c_str(), value.size(), nullptr, 0);
    if (size == 0) return L"";

    std::wstring wide(static_cast<size_t>(size), L'\0');
	utf8_to_utf16(value.c_str(), value.size(), wide.data(), size);
    return wide;
}
