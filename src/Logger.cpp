#include "Logger.h"

#include "platform.h"

#include <chrono>
#include <cstdarg>
#include <cwchar>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger g_logger;

static constexpr size_t kMaxRetainedLogLines = 1000;

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
static std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";

    const size_t size = utf16_to_utf8(value.c_str(), value.size(), nullptr, 0);
    if (size == 0) return "";

    std::string narrow(size, '\0');
    utf16_to_utf8(value.c_str(), value.size(), narrow.data(), size);
    return narrow;
}
#endif

void Logger::SetMinimumLevel(Level level) {
    minimumLevel_.store(LevelPriority(level), std::memory_order_relaxed);
}

Logger::LogHistory Logger::AddLogReflector(LogReflectorCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (callback != nullptr && std::find(logReflectors_.begin(), logReflectors_.end(), callback) == logReflectors_.end()) {
        logReflectors_.push_back(callback);
    }
    return LogHistory(recentLogLines_.begin(), recentLogLines_.end());
}

void Logger::RemoveLogReflector(LogReflectorCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    logReflectors_.erase(std::remove(logReflectors_.begin(), logReflectors_.end(), callback), logReflectors_.end());
}

void Logger::log(const wchar_t* function, Level level, const wchar_t* message, ...) {
    if (!ShouldLog(level)) return;

    va_list args;
    va_start(args, message);
    logV(function, level, message, args);
    va_end(args);
}

void Logger::log(const char* function, Level level, const char* message, ...) {
    if (!ShouldLog(level)) return;

    va_list args;
    va_start(args, message);
    logV(function, level, message, args);
    va_end(args);
}

void Logger::log(const char* function, Level level, const wchar_t* message, ...) {
    if (!ShouldLog(level)) return;

    std::wstring functionW = Utf8ToWide(function != nullptr ? function : "");
    va_list args;
    va_start(args, message);
    logV(functionW.c_str(), level, message, args);
    va_end(args);
}

void Logger::log(const wchar_t* function, Level level, const char* message, ...) {
    if (!ShouldLog(level)) return;

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


    // Pre-resolve strings once so we don't evaluate them twice
    std::wstring tsStr = timestamp.str();
    std::wstring lvlStr = LevelToString(level);
    std::wstring fnStr = (function != nullptr ? function : L"");
    std::wstring msgStr = (message != nullptr ? message : L"<null>");

    std::lock_guard<std::mutex> lock(mutex_);

    // Console Output (With ANSI Colors)
    std::wstringstream wstrstr;
    wstrstr << L"\x1b[0;90m" << tsStr
        << LevelToColor(level) << L" [" << lvlStr << L"] "
        << L"\x1b[0;90m" << L"[" << fnStr << L"] "
        << ResetColor() << msgStr << std::endl;
	std::wstring wstr = wstrstr.str();

    recentLogLines_.push_back(wstr);
    while (recentLogLines_.size() > kMaxRetainedLogLines) {
        recentLogLines_.pop_front();
    }

    std::wcout << wstr;
#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
    const std::wstring debugStr = tsStr + L" [" + lvlStr + L"] [" + fnStr + L"] " + msgStr + L"\n";
    const std::string debugUtf8 = WideToUtf8(debugStr);
    if (!debugUtf8.empty()) {
        std::fputs(debugUtf8.c_str(), stderr);
        std::fflush(stderr);
    }
#endif
    for (const auto& reflector : logReflectors_) {
        reflector(wstr);
    }

    #ifdef _WIN32
        // Debugger Output (Raw, No Colors)
        // Only pay the string concatenation cost if a debugger is actually listening
        if (IsDebuggerPresent()) {
            std::wstring debugStr = tsStr + L" [" + lvlStr + L"] [" + fnStr + L"] " + msgStr + L"\n";
            OutputDebugStringW(debugStr.c_str());
        }
    #endif
}

bool Logger::ShouldLog(Level level) const {
    return LevelPriority(level) >= minimumLevel_.load(std::memory_order_relaxed);
}

int Logger::LevelPriority(Level level) {
    switch (level) {
    case Level::DDebug:
        return 0;
    case Level::Debug:
        return 1;
    case Level::Info:
        return 2;
    case Level::Warning:
        return 3;
    case Level::Error:
        return 4;
    default:
        return 0;
    }
}

const wchar_t* Logger::LevelToString(Level level) {
    switch (level) {
    case Level::DDebug:
        return L"ddebug";
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
    case Level::DDebug:
        return L"\x1b[0;2;36m";   // Dim cyan for very chatty diagnostics
    case Level::Debug:
        return L"\x1b[0;36m";     // Cyan (Clear, but visually recedes slightly for spammy logs)
    case Level::Info:
        return L"\x1b[0;1;32m";   // Bold Green (Bright, positive confirmation)
    case Level::Warning:
        return L"\x1b[0;1;33m";   // Bold Yellow (High contrast, catches the eye)
    case Level::Error:
        return L"\x1b[0;1;31m";   // Bold Red (Maximum urgency)
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
