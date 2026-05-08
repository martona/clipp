#pragma once

#include <algorithm>
#include <cstdarg>
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
    void AddLogReflector(LogReflectorCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(logReflectors_.begin(), logReflectors_.end(), callback) == logReflectors_.end()) {
            logReflectors_.push_back(callback);
        }
    }
    void RemoveLogReflector(LogReflectorCallback callback) {
		std::lock_guard<std::mutex> lock(mutex_);
        logReflectors_.erase(std::remove(logReflectors_.begin(), logReflectors_.end(), callback), logReflectors_.end());
	}

    void log(const wchar_t* function, Level level, const wchar_t* message, ...);
    void log(const char* function, Level level, const char* message, ...);
    void log(const char* function, Level level, const wchar_t* message, ...);
    void log(const wchar_t* function, Level level, const char* message, ...);

private:
    void logV(const char* function, Level level, const char* message, va_list args);
    void logV(const wchar_t* function, Level level, const wchar_t* message, va_list args);
    void writeLine(const wchar_t* function, Level level, const wchar_t* message);
    static const wchar_t* LevelToString(Level level);
    static std::wstring Utf8ToWide(const std::string& value);
	static const wchar_t* LevelToColor(Level level);
    static const wchar_t* ResetColor();
	std::vector<LogReflectorCallback> logReflectors_;

    std::mutex mutex_;
};

extern Logger g_logger;
