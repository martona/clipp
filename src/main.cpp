// clipp.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <cstdlib>
#include <iostream>
#include <sodium.h>
#include <string>
#include <string_view>

#include "platform.h"

#include "Logger.h"
#include "Cli.h"
#include "KeyManager.h"
#include "NetworkRuntime.h"
#include "Peer.h"
#include "PeerManager.h"
#include "PeerDisplay.h"
#include "Clipboard.h"
#include "ClipboardActivityStore.h"
#include "ClipboardWire.h"
#include "CryptoChannel.h"
#include "LocalPeerName.h"
#include "platform/uistrings.h"
#include "Settings.h"
#include "utils.h"

#ifdef _WIN32
    #include <io.h>
    #include <fcntl.h>
    #include <Windows.h>
#else
    #include <termios.h>
    #include <unistd.h>
    #include <signal.h>
    #include <pthread.h>
    #include <thread>
#endif

Settings g_settings;
PeerDisplay g_peerDisplay;
PeerManager g_peerManager;
NetworkRuntime g_networkRuntime;
ClipboardActivityStore g_clipboardActivityStore;

#ifdef _WIN32
    namespace {
    constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\ClippSingleInstanceMutex";
    constexpr wchar_t kTrayWindowClassName[] = L"ClippHiddenTrayWindow";
    constexpr wchar_t kShowMainWindowMessageName[] = L"ClippShowMainWindow";
    }

    static void RequestRunningInstanceToShowWindow() {
        const UINT showMainWindowMessage = RegisterWindowMessageW(kShowMainWindowMessageName);
        if (showMainWindowMessage == 0) {
            return;
        }

        constexpr DWORD kTotalWaitMillis = 2000;
        constexpr DWORD kPollIntervalMillis = 50;
        for (DWORD waitedMillis = 0; waitedMillis <= kTotalWaitMillis; waitedMillis += kPollIntervalMillis) {
            HWND trayWindow = FindWindowExW(HWND_MESSAGE, nullptr, kTrayWindowClassName, nullptr);
            if (trayWindow) {
                PostMessageW(trayWindow, showMainWindowMessage, 0, 0);
                return;
            }
            Sleep(kPollIntervalMillis);
        }
    }

    SingleInstanceResult EnsureSingleInstance() {
        HANDLE newMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
        if (!newMutex) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, L"CreateMutexW failed while creating the single-instance mutex (GetLastError=%lu).", GetLastError());
            return SingleInstanceResult::ExitFailure;
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(newMutex);
            RequestRunningInstanceToShowWindow();
            return SingleInstanceResult::ExitSuccess;
        }

        return SingleInstanceResult::Continue;
    }

    void StopSingleInstanceServer() {
    }

    void TrayIconMessageLoop(bool showNetworkPageOnStartup);
    void TrayIconShutdown();
    BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
        if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT || dwCtrlType == CTRL_CLOSE_EVENT) {
            g_logger.log(__FUNCTION__, Logger::Level::Info, "Console control event received, shutting down...");
            TrayIconShutdown();
            return TRUE;
        }
        return FALSE;
    }

    #pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
SingleInstanceResult EnsureSingleInstance() {
    return SingleInstanceResult::Continue;
}

void StopSingleInstanceServer() {
}

bool RegisterClippAutoStart() {
    return true;
}

bool UnregisterClippAutoStart() {
    return true;
}
#endif

bool InitializeConsoleOutput() {
    #ifdef _WIN32
    // Attempt to attach to the console of the process that launched this app (e.g., cmd.exe or pwsh.exe).
    // If launched from Explorer (double-click), there is no parent console, and this fails.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {

        // The console is attached, but C/C++ standard streams still point to the void. 
        // We must map them directly to the console output buffer.
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);

        // Clear the state of C++ streams and sync them to the newly mapped C streams
        std::wcout.clear();
        std::cout.clear();
        std::wcerr.clear();
        std::cerr.clear();
        std::ios::sync_with_stdio(true);

        freopen_s(&fp, "CONIN$", "r", stdin);
        std::wcin.clear();
        std::cin.clear();

        // Enable ANSI color escape codes for this attached terminal
        HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;
        if (GetConsoleMode(hStdout, &mode)) {
            SetConsoleMode(hStdout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING); 
        }

        // Output a blank line to ensure the prompt cursor is pushed down cleanly
        std::cout << std::endl;
        return true;
    }
    return false;
#else
    // On macOS, if the app is launched from LaunchServices (Finder/Dock), stdout is redirected 
    // to /dev/null or the system log. If launched from a terminal, stdout is an active TTY.
    return isatty(STDOUT_FILENO) != 0;
#endif
}

void OnClipboardNotification(PlatformWindowHandle hwnd) {
    g_logger.log(__FUNCTION__, Logger::Level::Debug, "Clipboard notification received");
    auto clipboardData = ReadClipboardData(hwnd);
    if (clipboardData.meta.formatId == CLIPP_FORMAT_NONE) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "Clipboard is empty, contains unsupported format, or came from us");
        return;
    }

    // If the source app marked the clipboard content as private and the user
    // setting honors those markers, replace the payload with an empty UTF-8
    // placeholder that carries only the privacy flag. Peers receiving it will
    // show a "marked private — sync skipped" entry in the activity stream and
    // will not touch their local clipboard.
    const bool sourceMarkedPrivate =
        (clipboardData.meta.flags & NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE) != 0;
    if (sourceMarkedPrivate && g_settings.honorExternalPrivacyMarkers()) {
        g_logger.log(__FUNCTION__, Logger::Level::Info,
            "Source marked clipboard content as private and 'honor markers' setting is on; sending empty placeholder.");
        ClipboardPayload placeholder;
        placeholder.meta.formatId = CLIPP_FORMAT_UTF8;
        placeholder.meta.flags = NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE;
        placeholder.SetUncompressedBytes({});
        clipboardData = std::move(placeholder);
    }

    HostId localHostId;
    g_settings.getHostID(localHostId);  // zero-init HostId on failure is fine for the activity record
    const std::string localHostName = clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);
    clipboardData.StampOrigin(localHostId, localHostName.c_str(), g_settings.nextOriginSequenceNumber());
    auto payload = std::make_shared<const ClipboardPayload>(std::move(clipboardData));
    g_clipboardActivityStore.Add(payload);
    g_peerManager.BroadcastClipboard(payload);
    g_logger.log(__FUNCTION__, Logger::Level::Debug, "Broadcast clipboard data to peers (format: %s, ID: %u, encoded size: %zu bytes, uncompressed size: %llu bytes)",
        ClippClipboardFormatName(payload->meta.formatId),
        payload->meta.formatId,
        payload->EncodedBytes().size(),
        static_cast<unsigned long long>(payload->meta.uncompressedDataSize));
}

int main(int argc, char* argv[]) {

    InitializeConsoleOutput();

    // Command-line mode: if a recognized subcommand (key/hostid) ran, return its
    // exit code. Otherwise any global options (e.g. --loglevel) have been applied
    // and we fall through to the GUI. Run() installs the crash handler internally
    // once the log level is decided, so its "installed" line stays out of command
    // output while still guarding the GUI loop below.
    if (auto commandExitCode = clipp::cli::Run(argc, argv)) {
        return *commandExitCode;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Info, L"==================================================================");

    switch (EnsureSingleInstance()) {
        case SingleInstanceResult::Continue:
            break;
        case SingleInstanceResult::ExitSuccess:
            return 0;
        case SingleInstanceResult::ExitFailure:
            return 1;
    }
    const auto exitAfterStartupFailure = [](int exitCode) {
        StopSingleInstanceServer();
        return exitCode;
    };

    RegisterClippAutoStart();

    #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return exitAfterStartupFailure(-1);
        }
    #else
        // ignore SIGPIPE to prevent crashes when writing to a closed socket
        signal(SIGPIPE, SIG_IGN);
        // block SIGINT and SIGTERM in all threads; we'll handle shutdown
        sigset_t waitset;
        sigemptyset(&waitset);
        sigaddset(&waitset, SIGINT);
        sigaddset(&waitset, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &waitset, nullptr);
    #endif

    // libsodium requires initialization before calling any other functions
    if (!InitializeSodium()) {
        return exitAfterStartupFailure(1);
    }

    HostId hostID;
    if (!g_settings.ensureHostID(hostID)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Fatal: failed to initialize host ID.");
        return exitAfterStartupFailure(1);
    }

    g_clipboardActivityStore.SetLimits(
        g_settings.clipboardHistoryMemoryLimitBytes(),
        g_settings.clipboardHistoryMaxAgeSeconds(),
        g_settings.clipboardHistoryMaxItems());

    std::string keyErrorMessage;
    const std::wstring networkFingerprint = g_keyManager.GetNetworkFingerprintHash(nullptr, &keyErrorMessage);
    const bool haveNetworkKey = !networkFingerprint.empty();
    if (haveNetworkKey) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, L"Network fingerprint: %ls", networkFingerprint.c_str());
    } else {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, "No network key configured yet: %s", keyErrorMessage.c_str());
    }

    if (!StartClipboardNotification(OnClipboardNotification)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to start clipboard notification thread!");
    }

    if (!g_networkRuntime.Start()) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to start network runtime thread!");
    } else {
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Successfully started.");
    }

    #ifdef _WIN32
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        TrayIconMessageLoop(!haveNetworkKey);
    #elif defined(__APPLE__)
        std::thread signalThread([waitset]() mutable {
            int caughtSignal = 0;
            if (sigwait(&waitset, &caughtSignal) == 0) {
                RequestMacOSAppShutdown();
            }
        });
        signalThread.detach();
        RunMacOSStatusMenu(!haveNetworkKey);
    #else
        int caughtSignal;
        sigwait(&waitset, &caughtSignal);
    #endif

    g_networkRuntime.Stop();
    g_logger.log(__FUNCTION__, Logger::Level::Info, "Network runtime stopped.");

    StopClipboardNotification();
    g_logger.log(__FUNCTION__, Logger::Level::Info, "Clipboard notification stopped.");

    g_peerManager.ClearPeers();
    g_logger.log(__FUNCTION__, Logger::Level::Info, "Peer manager cleared.");

    StopSingleInstanceServer();

    #ifdef _WIN32
        WSACleanup();
    #endif

    return 0;
}
