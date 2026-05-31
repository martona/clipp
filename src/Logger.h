#pragma once

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Logger {
public:
    enum class Level {
        DDebug,
        Debug,
        Info,
        Warning,
        Error,
        // Sentinel above Error; used only as a minimum threshold to silence all
        // output (e.g. command-line mode). No message is ever logged at this level.
        Off,
    };

    using LogReflectorCallback = void(*)(const std::wstring& line);
    using LogHistory = std::vector<std::wstring>;

    ~Logger();

    void SetMinimumLevel(Level level);

    LogHistory AddLogReflector(LogReflectorCallback callback);
    void RemoveLogReflector(LogReflectorCallback callback);

    // Per-launch file logging. The caller passes a UTF-8 directory (resolved via
    // platform/LogPaths.h); EnableFileLogging sweeps it for files older than
    // kDefaultRetentionDays on entry, then the log file is created lazily on the
    // first emitted line and rolls over at local midnight. Idempotent. Safe to
    // never call -- the default is in-memory + stderr only.
    void EnableFileLogging(const std::string& utf8Dir);

    // Retention sweep shared by the log directory (.log) and the crash-dump
    // directory (.dmp, see win32/CrashHandler.cpp): deletes files in utf8Dir named
    // "<prefix>...<extension>" older than retentionDays. Age is taken from the
    // embedded clipp-YYYYMMDD- datestamp, falling back to the file's mtime. Static,
    // self-contained, and never throws.
    static void PruneAgedFiles(const std::string& utf8Dir,
                               const std::string& prefix,
                               const std::string& extension,
                               int retentionDays);

    // Retention window (days) applied to both rolling logs and crash dumps.
    static constexpr int kDefaultRetentionDays = 10;

    void log(const wchar_t* function, Level level, const wchar_t* message, ...);
    void log(const char* function, Level level, const char* message, ...);
    void log(const char* function, Level level, const wchar_t* message, ...);
    void log(const wchar_t* function, Level level, const char* message, ...);

private:
    void logV(const char* function, Level level, const char* message, va_list args);
    void logV(const wchar_t* function, Level level, const wchar_t* message, va_list args);
    void writeLine(const wchar_t* function, Level level, const wchar_t* message);
    void WriteToFileLocked(const std::wstring& plainLine);  // caller must hold mutex_
    bool ShouldLog(Level level) const;
    static int LevelPriority(Level level);
    static const wchar_t* LevelToString(Level level);
    static std::wstring Utf8ToWide(const std::string& value);
	static const wchar_t* LevelToColor(Level level);
    static const wchar_t* ResetColor();
	std::vector<LogReflectorCallback> logReflectors_;
    std::deque<std::wstring> recentLogLines_;

    std::mutex mutex_;
    std::atomic<int> minimumLevel_{ 1 };

    // File sink (null unless EnableFileLogging was called). Defined in Logger.cpp so
    // <filesystem>/<fstream> stay out of this widely-included header.
    struct FileSink;
    std::unique_ptr<FileSink> fileSink_;
};

extern Logger g_logger;
