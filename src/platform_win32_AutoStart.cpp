#include "platform_win32_AutoStart.h"

#include "Logger.h"

#include <Windows.h>

#include <string>

namespace {
    constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunValueName[] = L"Clipp";
    constexpr DWORD kMaxModulePathLength = 32768;
}

static std::wstring GetCurrentExecutablePath() {
    DWORD bufferLength = MAX_PATH;
    for (;;) {
        std::wstring path(bufferLength, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), bufferLength);
        if (length == 0) {
            return {};
        }

        if (length < bufferLength) {
            path.resize(length);
            return path;
        }

        if (bufferLength >= kMaxModulePathLength) {
            return {};
        }
        bufferLength *= 2;
    }
}

static std::wstring MakeAutoStartCommand() {
    const std::wstring executablePath = GetCurrentExecutablePath();
    if (executablePath.empty()) {
        return {};
    }

    return L"\"" + executablePath + L"\"";
}

bool RegisterClippAutoStart() {
    const std::wstring command = MakeAutoStartCommand();
    if (command.empty()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to resolve executable path for startup registration.");
        return false;
    }

    HKEY runKey = nullptr;
    LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &runKey, nullptr);
    if (status != ERROR_SUCCESS) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to open startup registry key (status=%ld).", status);
        return false;
    }

    status = RegSetValueExW(runKey,
        kRunValueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(runKey);

    if (status != ERROR_SUCCESS) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to register startup command (status=%ld).", status);
        return false;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Registered startup command: %ls", command.c_str());
    return true;
}

bool UnregisterClippAutoStart() {
    HKEY runKey = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &runKey);
    if (status == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (status != ERROR_SUCCESS) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to open startup registry key for removal (status=%ld).", status);
        return false;
    }

    status = RegDeleteValueW(runKey, kRunValueName);
    RegCloseKey(runKey);

    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to unregister startup command (status=%ld).", status);
        return false;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Unregistered startup command.");
    return true;
}
