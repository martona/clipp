#include "AutoStart.h"

#include "Logger.h"

#include <Windows.h>
#include <appmodel.h>

#include <string>
#include <thread>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>

namespace {
    constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunValueName[] = L"Clipp";
    constexpr DWORD kMaxModulePathLength = 32768;

    // Must match the TaskId in the MSIX manifest's windows.startupTask extension.
    constexpr wchar_t kStartupTaskId[] = L"ClippAutoStart";

    // True when running with MSIX package identity (Store / sideloaded). Unpackaged
    // Developer-ID / GitHub builds get APPMODEL_ERROR_NO_PACKAGE.
    bool RunningPackaged() {
        UINT32 length = 0;
        return ::GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
    }
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

namespace {
    // Under MSIX the app's HKCU writes go to a per-package copy-on-write overlay that the
    // logon scan never sees, so the Run-key path below is inert when packaged. The
    // manifest's windows.startupTask is the autostart that actually fires; drive it
    // through the StartupTask WinRT API instead.

    // Fire-and-forget enable. Runs on a thread-pool (MTA) thread that outlives the call,
    // so RequestEnableAsync completes on its own -- a WinRT async op isn't cancelled by
    // dropping its handle. We neither wait nor inspect the result: if it's DisabledByUser
    // this is a silent no-op, which is exactly the behavior we want.
    winrt::fire_and_forget EnableStartupTaskAsync() {
        try {
            co_await winrt::resume_background();
            auto task = co_await winrt::Windows::ApplicationModel::StartupTask::GetAsync(kStartupTaskId);
            task.RequestEnableAsync();
        }
        catch (...) {
        }
    }

    // Blocking disable, used on exit: it must finish before the process dies or the state
    // never flips. Run on a short-lived MTA thread and join -- MTA sidesteps the STA
    // marshaling deadlock a blocking wait would hit on the UI thread.
    void DisableStartupTaskBlocking() {
        std::thread([] {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                auto task = winrt::Windows::ApplicationModel::StartupTask::GetAsync(kStartupTaskId).get();
                task.Disable();
            }
            catch (...) {
            }
        }).join();
    }
}

bool RegisterClippAutoStart() {
    if (RunningPackaged()) {
        // Packaged: enable the manifest startupTask; the HKCU Run key is inert under MSIX.
        EnableStartupTaskAsync();
        return true;
    }

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
    if (RunningPackaged()) {
        // Packaged: disable the manifest startupTask, synchronously, before we exit.
        DisableStartupTaskBlocking();
        return true;
    }

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
