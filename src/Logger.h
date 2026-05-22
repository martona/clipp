#pragma once

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class Logger {
public:
    enum class Level {
        Debug,
        Info,
        Warning,
        Error,
    };

    using LogReflectorCallback = void(*)(const std::wstring& line);
    using LogHistory = std::vector<std::wstring>;

    void SetMinimumLevel(Level level);

    LogHistory AddLogReflector(LogReflectorCallback callback);
    void RemoveLogReflector(LogReflectorCallback callback);

    void log(const wchar_t* function, Level level, const wchar_t* message, ...);
    void log(const char* function, Level level, const char* message, ...);
    void log(const char* function, Level level, const wchar_t* message, ...);
    void log(const wchar_t* function, Level level, const char* message, ...);

private:
    void logV(const char* function, Level level, const char* message, va_list args);
    void logV(const wchar_t* function, Level level, const wchar_t* message, va_list args);
    void writeLine(const wchar_t* function, Level level, const wchar_t* message);
    bool ShouldLog(Level level) const;
    static int LevelPriority(Level level);
    static const wchar_t* LevelToString(Level level);
    static std::wstring Utf8ToWide(const std::string& value);
	static const wchar_t* LevelToColor(Level level);
    static const wchar_t* ResetColor();
	std::vector<LogReflectorCallback> logReflectors_;
    std::deque<std::wstring> recentLogLines_;

    std::mutex mutex_;
    std::atomic<int> minimumLevel_{ 0 };
};

extern Logger g_logger;
