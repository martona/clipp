#include "CrashHandler.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <new>
#include <string>

#include "Logger.h"
#include "platform.h"  // clipp_platform_detail::Utf16ToUtf8String

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

// Fixed-size storage for the dump directory. Populated once at install time
// while the heap is healthy, then read from inside crash handlers without
// allocating. Cap is generous enough for a deep LocalAppData path plus the
// "\Clipp\crashdumps" suffix.
constexpr size_t kDumpDirCapacity = 768;
wchar_t g_dumpDirectory[kDumpDirCapacity] = {};

// Guard so InstallCrashHandler is idempotent if accidentally called twice.
std::atomic<bool> g_installed{ false };

// Snapshot of the previous filter, in case something upstream wants the
// vectored chain. We don't currently invoke it — we'd rather just die after
// writing the dump — but holding the pointer leaves the option open.
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

bool ResolveCrashDumpDirectory(std::wstring& outDir) {
    PWSTR localAppData = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    if (FAILED(hr) || localAppData == nullptr) {
        if (localAppData != nullptr) {
            CoTaskMemFree(localAppData);
        }
        return false;
    }
    outDir.assign(localAppData);
    CoTaskMemFree(localAppData);
    outDir.append(L"\\Clipp\\crashdumps");
    return true;
}

bool EnsureDirectoryExists(const std::wstring& path) {
    // Walk forward through the path and CreateDirectoryW each segment. Each
    // call returns ERROR_ALREADY_EXISTS for segments that exist, which we
    // intentionally ignore. The final check confirms the leaf is a directory.
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] == L'\\') {
            const std::wstring segment = path.substr(0, i);
            CreateDirectoryW(segment.c_str(), nullptr);
        }
    }
    CreateDirectoryW(path.c_str(), nullptr);

    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Writes a minidump and returns true on success. Designed to be safe to call
// from inside a crash handler: no heap allocation (uses stack buffers and the
// pre-populated global directory), no logging that might re-enter the heap or
// the file system in unexpected ways.
bool WriteMinidumpUnsafe(EXCEPTION_POINTERS* exceptionPointers) {
    if (g_dumpDirectory[0] == L'\0') {
        return false;
    }

    wchar_t filename[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    const int written = _snwprintf_s(
        filename,
        _countof(filename),
        _TRUNCATE,
        L"%ls\\clipp-%04u%02u%02u-%02u%02u%02u-%lu.dmp",
        g_dumpDirectory,
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        GetCurrentProcessId());
    if (written < 0) {
        return false;
    }

    const HANDLE hFile = CreateFileW(
        filename,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = exceptionPointers;
    mei.ClientPointers = FALSE;

    // Moderate dump: enough to symbolicate a stack across all threads and see
    // referenced memory, without dumping the entire address space.
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs
        | MiniDumpWithIndirectlyReferencedMemory
        | MiniDumpWithThreadInfo
        | MiniDumpWithUnloadedModules
        | MiniDumpWithProcessThreadData);

    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        type,
        exceptionPointers != nullptr ? &mei : nullptr,
        nullptr,
        nullptr);

    CloseHandle(hFile);
    return ok != FALSE;
}

LONG WINAPI ClippUnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    WriteMinidumpUnsafe(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

void TerminateHandler() {
    // std::terminate can be invoked without an active SEH exception (e.g.,
    // uncaught C++ exception that doesn't map to a structured one). Capture
    // the current context so the dump still has a thread state to walk.
    EXCEPTION_RECORD ex{};
    ex.ExceptionCode = STATUS_NONCONTINUABLE_EXCEPTION;
    ex.ExceptionFlags = EXCEPTION_NONCONTINUABLE;

    CONTEXT ctx{};
    RtlCaptureContext(&ctx);

    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &ex;
    ep.ContextRecord = &ctx;

    WriteMinidumpUnsafe(&ep);

    // Bypass abort()'s dialog/report behavior; we've already captured what we
    // need.
    TerminateProcess(GetCurrentProcess(), STATUS_NONCONTINUABLE_EXCEPTION);
}

void PurecallHandler() {
    // Re-route through the SEH filter so we get a consistent dump path.
    RaiseException(EXCEPTION_NONCONTINUABLE_EXCEPTION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void InvalidParameterHandler(
    const wchar_t* /*expression*/,
    const wchar_t* /*function*/,
    const wchar_t* /*file*/,
    unsigned int /*line*/,
    uintptr_t /*pReserved*/)
{
    RaiseException(EXCEPTION_NONCONTINUABLE_EXCEPTION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

// Vectored exception handlers fire BEFORE the OS routes an exception to the
// unhandled-exception filter or any SEH __try/__except. They see exception
// codes that bypass SetUnhandledExceptionFilter entirely — most importantly
// STATUS_FAIL_FAST_EXCEPTION, which is what RoFailFastWithErrorContext raises
// when a C++ exception escapes into the WinRT/COM runtime. Without this
// handler, such crashes terminate the process silently with no dump.
//
// Filtered to only the fatal codes that bypass the standard chain — we don't
// want to fire on every benign SEH (page faults, expected __try/__except, the
// C++ EH code for thrown exceptions that user code catches normally, etc.),
// both because that's wasted work and because we'd double-dump for AVs that
// the existing UnhandledExceptionFilter already handles.
//
// Codes are hex literals to avoid pulling in ntstatus.h (which conflicts with
// the partial set of STATUS_* macros that winnt.h already defines). The values
// are stable across Windows versions.
LONG WINAPI VectoredExceptionHandler(EXCEPTION_POINTERS* ep) {
    if (ep == nullptr || ep->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    switch (ep->ExceptionRecord->ExceptionCode) {
        case 0xC0000602u: // STATUS_FAIL_FAST_EXCEPTION (RoFailFastWithErrorContext, RaiseFailFastException)
        case 0xC0000409u: // STATUS_STACK_BUFFER_OVERRUN (/GS cookie failure, __fastfail)
        case 0xC0000374u: // STATUS_HEAP_CORRUPTION (heap manager fatal)
            WriteMinidumpUnsafe(ep);
            break;
        default:
            break;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

namespace clipp {

void InstallCrashHandler() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        return;
    }

    std::wstring dir;
    if (!ResolveCrashDumpDirectory(dir)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Could not resolve LocalAppData; minidumps will not be written.");
        return;
    }

    if (!EnsureDirectoryExists(dir)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Could not create crash-dump directory; minidumps will not be written: %ls",
            dir.c_str());
        return;
    }

    if (dir.size() >= kDumpDirCapacity) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Crash-dump directory path is too long (%zu chars); minidumps will not be written: %ls",
            dir.size(), dir.c_str());
        return;
    }

    // Minidumps can contain clipboard contents and other process memory, so don't let
    // them accumulate. Sweep the same retention window the logger uses, keyed off the
    // shared clipp-YYYYMMDD- name. The heap is healthy here (install time), so the
    // filesystem work is safe; PruneAgedFiles never throws.
    Logger::PruneAgedFiles(clipp_platform_detail::Utf16ToUtf8String(dir), "clipp-", ".dmp", Logger::kDefaultRetentionDays);

    std::wmemcpy(g_dumpDirectory, dir.c_str(), dir.size() + 1);

    g_previousFilter = SetUnhandledExceptionFilter(ClippUnhandledExceptionFilter);
    std::set_terminate(TerminateHandler);
    _set_purecall_handler(PurecallHandler);
    _set_invalid_parameter_handler(InvalidParameterHandler);

    // Vectored handler catches the silent fast-fail paths that bypass the
    // unhandled-exception filter — chiefly WinRT/COM-mediated terminations
    // where a C++ exception escapes a XAML/dispatcher callback and the
    // runtime calls RoFailFastWithErrorContext.
    AddVectoredExceptionHandler(0 /* append; called last among vectored handlers */,
                                VectoredExceptionHandler);

    // Suppress the "this app has stopped working" dialog so the crash is quiet
    // and the dump file is released promptly.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    g_logger.log(__FUNCTION__, Logger::Level::Info,
        L"Crash handler installed; minidumps will be written to %ls",
        g_dumpDirectory);
}

} // namespace clipp

#else // !_WIN32

namespace clipp {
void InstallCrashHandler() {}
} // namespace clipp

#endif
