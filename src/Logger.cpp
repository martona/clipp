#include "Logger.h"

#include "platform.h"

#include <chrono>
#include <cstdarg>
#include <cwchar>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#if !defined(_WIN32)
#include <unistd.h>  // getpid for the per-launch log filename
#endif

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

namespace {

namespace fs = std::filesystem;

// Build a filesystem path from a UTF-8 string with the correct per-OS encoding. On
// Windows the native path encoding is UTF-16, so convert; on POSIX the native narrow
// encoding is already UTF-8.
fs::path PathFromUtf8(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) {
        return fs::path();
    }
    const size_t size = utf8_to_utf16(utf8.c_str(), utf8.size(), nullptr, 0);
    if (size == 0) {
        return fs::path();
    }
    std::wstring wide(size, L'\0');
    utf8_to_utf16(utf8.c_str(), utf8.size(), wide.data(), size);
    return fs::path(wide);
#else
    return fs::path(utf8);
#endif
}

unsigned long CurrentProcessId() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

// Local-midnight day index for a time_t (days since the epoch in local time). Lets
// us compare calendar ages without pulling in C++20 calendar facilities.
long long LocalDayNumber(std::time_t t) {
    std::tm tm{};
    localtime_safe(&tm, &t);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    const std::time_t midnight = std::mktime(&tm);
    return (midnight == static_cast<std::time_t>(-1))
        ? 0
        : static_cast<long long>(midnight / 86400);
}

} // namespace

// Per-launch file sink. Defined out-of-line so the heavy <filesystem>/<fstream>
// includes stay confined to this translation unit (see Logger.h).
struct Logger::FileSink {
    std::string utf8Dir;          // retained for the midnight-rollover re-sweep
    fs::path dir;
    std::ofstream stream;
    int fileYear = 0;
    int fileMonth = 0;
    int fileDay = 0;
    bool openFailed = false;      // give up quietly after a failed open
};

Logger::~Logger() = default;

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

void Logger::EnableFileLogging(const std::string& utf8Dir) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fileSink_) {
            return;  // idempotent
        }
        auto sink = std::make_unique<FileSink>();
        sink->utf8Dir = utf8Dir;
        sink->dir = PathFromUtf8(utf8Dir);
        fileSink_ = std::move(sink);
    }
    // Startup retention sweep, performed outside the lock so we don't hold it during
    // filesystem I/O. Runs even if this launch never opens a file -- that satisfies
    // "enumerate and clean on startup" regardless of whether anything is logged.
    PruneAgedFiles(utf8Dir, "clipp-", ".log", kDefaultRetentionDays);
}

void Logger::PruneAgedFiles(const std::string& utf8Dir,
                            const std::string& prefix,
                            const std::string& extension,
                            int retentionDays) {
    if (retentionDays < 0) {
        return;
    }
    try {
        std::error_code ec;
        const fs::path dir = PathFromUtf8(utf8Dir);
        if (!fs::is_directory(dir, ec)) {
            return;  // first launch (dir not created yet) or unreadable -- nothing to do
        }

        const long long today = LocalDayNumber(std::time(nullptr));

        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            try {
                const fs::path& path = it->path();
                std::error_code fec;
                if (!fs::is_regular_file(path, fec) || fec) {
                    continue;
                }

                const std::string name = path.filename().string();
                if (name.size() < prefix.size() + extension.size()) {
                    continue;
                }
                if (name.compare(0, prefix.size(), prefix) != 0) {
                    continue;
                }
                if (name.compare(name.size() - extension.size(), extension.size(), extension) != 0) {
                    continue;
                }

                // Prefer the embedded clipp-YYYYMMDD- datestamp; it survives backup
                // tools and SMB mounts bumping mtime. Fall back to mtime only if the
                // name doesn't carry a parseable date.
                long long fileDay = -1;
                if (name.size() >= prefix.size() + 8) {
                    const std::string ymd = name.substr(prefix.size(), 8);
                    const bool allDigits = std::all_of(ymd.begin(), ymd.end(),
                        [](unsigned char c) { return c >= '0' && c <= '9'; });
                    if (allDigits) {
                        std::tm tm{};
                        tm.tm_year = std::stoi(ymd.substr(0, 4)) - 1900;
                        tm.tm_mon = std::stoi(ymd.substr(4, 2)) - 1;
                        tm.tm_mday = std::stoi(ymd.substr(6, 2));
                        tm.tm_isdst = -1;
                        const std::time_t t = std::mktime(&tm);
                        if (t != static_cast<std::time_t>(-1)) {
                            fileDay = static_cast<long long>(t / 86400);
                        }
                    }
                }

                if (fileDay < 0) {
                    std::error_code wec;
                    const auto ftime = fs::last_write_time(path, wec);
                    if (wec) {
                        continue;
                    }
                    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                    fileDay = LocalDayNumber(std::chrono::system_clock::to_time_t(sys));
                }

                if (today - fileDay > retentionDays) {
                    std::error_code rec;
                    fs::remove(path, rec);  // best-effort; ignore failures
                }
            } catch (...) {
                continue;  // a single bad entry must not abort the whole sweep
            }
        }
    } catch (...) {
        // Contract: never throw. Retention is best-effort.
    }
}

void Logger::WriteToFileLocked(const std::wstring& plainLine) {
    if (!fileSink_ || fileSink_->openFailed) {
        return;
    }
    FileSink& sink = *fileSink_;

    // Local date drives both the filename and the midnight rollover check.
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_safe(&tm, &now);
    const int year = tm.tm_year + 1900;
    const int month = tm.tm_mon + 1;
    const int day = tm.tm_mday;

    if (sink.stream.is_open() &&
        (year != sink.fileYear || month != sink.fileMonth || day != sink.fileDay)) {
        // Crossed midnight on a long-running process: roll to a fresh file and
        // re-apply the retention window.
        sink.stream.close();
        PruneAgedFiles(sink.utf8Dir, "clipp-", ".log", kDefaultRetentionDays);
    }

    if (!sink.stream.is_open()) {
        std::error_code ec;
        fs::create_directories(sink.dir, ec);  // best-effort; the open below is the real check

        // clipp-YYYYMMDD-HHMMSS-pid.log -- same convention as the crash dumps.
        char name[64];
        std::snprintf(name, sizeof(name), "clipp-%04d%02d%02d-%02d%02d%02d-%lu.log",
                      year, month, day, tm.tm_hour, tm.tm_min, tm.tm_sec, CurrentProcessId());

        sink.stream.open(sink.dir / name, std::ios::out | std::ios::app | std::ios::binary);
        if (!sink.stream.is_open()) {
            // Degrade silently: never log from here (we hold mutex_ -- re-entry would
            // deadlock) and never throw. The missing file is itself the signal.
            sink.openFailed = true;
            return;
        }
        sink.fileYear = year;
        sink.fileMonth = month;
        sink.fileDay = day;
    }

    const std::string utf8 = clipp_platform_detail::Utf16ToUtf8String(plainLine);
    sink.stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    sink.stream.flush();
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

    // Plain (no ANSI color) rendering, reused for the file sink, the Windows
    // redirected/debugger paths, and the iOS debug stream.
    const std::wstring plainLine = tsStr + L" [" + lvlStr + L"] [" + fnStr + L"] " + msgStr + L"\n";

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

    WriteToFileLocked(plainLine);

#ifdef _WIN32
    // On a console, write wide (renders the ANSI colors above). On a redirected
    // file/pipe, write clean UTF-8 without the color codes -- MSVC wide streams do
    // not reliably write to non-console targets, so a redirected log would
    // otherwise vanish entirely.
    {
        const HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        const bool stderrIsConsole =
            hErr && hErr != INVALID_HANDLE_VALUE && GetFileType(hErr) == FILE_TYPE_CHAR;
        if (stderrIsConsole) {
            std::wcerr << wstr;
        } else {
            std::cerr << clipp_platform_detail::Utf16ToUtf8String(plainLine);
        }
    }
#elif defined(__linux__)
    // Write UTF-8 bytes, not wide. std::wcerr under the default "C" locale sets
    // failbit on the first non-ASCII wide char (a device/network name) and then
    // silently drops ALL subsequent logging. Converting to UTF-8 is locale-
    // independent and preserves the ANSI color escapes (they're ASCII).
    std::cerr << clipp_platform_detail::Utf16ToUtf8String(wstr);
#else
    std::wcerr << wstr;
#endif
#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
    const std::string debugUtf8 = WideToUtf8(plainLine);
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
            OutputDebugStringW(plainLine.c_str());
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
    case Level::Off:
        return 5;
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
    case Level::Off:
        return L"off";
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
