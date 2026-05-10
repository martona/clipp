// clipp.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <cstdlib>
#include <iostream>
#include <sodium.h>
#include <string>

#include "platform.h"

#include "Logger.h"
#include "KeyManager.h"
#include "NetworkRuntime.h"
#include "Peer.h"
#include "PeerManager.h"
#include "PeerDisplay.h"
#include "Clipboard.h"
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

    void TrayIconMessageLoop();
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

static std::string ReadLine(const std::string & prompt) {
    std::cout << prompt.c_str();
    std::string input;
    std::getline(std::cin, input);
    return input;
}

static std::string ReadHiddenLine(const std::string & prompt) {
    std::cout << prompt.c_str();
    std::string input;

    #ifdef _WIN32
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hStdin, &mode);
        SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));

        std::getline(std::cin, input);

        SetConsoleMode(hStdin, mode);
    #else
        termios oldt;
        tcgetattr(STDIN_FILENO, &oldt);

        termios newt = oldt;
        newt.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);

        std::getline(std::cin, input);

        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    #endif

    std::cout << std::endl;
    return input;
}

void OnClipboardNotification(PlatformWindowHandle hwnd) {
    g_logger.log(__FUNCTION__, Logger::Level::Debug, "Clipboard notification received");
    auto clipboardData = ReadClipboardData(hwnd);
    if (clipboardData.formatId == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "Clipboard is empty or contains unsupported format");
        return;
	}
	const size_t decodedDataSize = clipboardData.rawData.size();
	if (!clipboardData.ZstdCompress()) {
		g_logger.log(__FUNCTION__, Logger::Level::Warning, "Failed to compress clipboard data; skipping broadcast");
		return;
	}
	auto payload = std::make_shared<const ClipboardPayload>(clipboardData);
    g_peerManager.BroadcastClipboard(payload);
	g_logger.log(__FUNCTION__, Logger::Level::Debug, "Broadcast clipboard data to peers (format ID: %u, encoded size: %zu bytes, decoded size: %zu bytes)", clipboardData.formatId, clipboardData.rawData.size(), decodedDataSize);
}

void PrintNetworkKeyHash(const std::array<unsigned char, KeyManager::NetworkKeySize>& networkKey) {
    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Network Key hash: %ls", g_keyManager.GetNetworkKeyHash(&networkKey).c_str());
}

int main(int argc, char* argv[]) {
	
    InitializeConsoleOutput();

    if (argc > 1 && std::string(argv[1]) == "setkey") {
        std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
        std::string keyInput = ReadHiddenLine("Enter a password to derive network key from: ");
		if (keyInput.empty()) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "No input provided.");
            return 1;
        }
        std::string netNameAndPassword = g_settings.networkName();
        netNameAndPassword += "|";
        netNameAndPassword += keyInput;
		bool keyDerived = g_keyManager.DeriveNetworkKey(netNameAndPassword, networkKey);
        sodium_memzero(keyInput.data(), keyInput.capacity());
        sodium_memzero(netNameAndPassword.data(), netNameAndPassword.capacity());
        if (!keyDerived) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to derive network key from password.");
            return 1;
		}

		PrintNetworkKeyHash(networkKey);

        std::string errorMessage;
        if (!g_keyManager.SetNetworkKey(networkKey, &errorMessage)) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to store network key: %s", errorMessage.c_str());
            return 1;
        }

        g_logger.log(__FUNCTION__, Logger::Level::Info, "Network key saved successfully.");
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "setname") {
        std::string input = ReadLine("Enter network name: ");
        if (input.empty()) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "No input provided.");
            return 1;
        }

        if (!g_settings.set_networkName(input)) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to store network name.");
            return 1;
        }

        g_logger.log(__FUNCTION__, Logger::Level::Info, "Network name saved successfully.");
        return 0;
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
    if (sodium_init() < 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Fatal: libsodium failed to initialize!");
        return exitAfterStartupFailure(1);
    }
    g_logger.log(__FUNCTION__, Logger::Level::Debug, "libsodium initialized successfully.");

    HostId hostID;
    if (!g_settings.ensureHostID(hostID)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Fatal: failed to initialize host ID.");
        return exitAfterStartupFailure(1);
    }

    std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
    std::string keyErrorMessage;
    if (g_keyManager.GetNetworkKey(networkKey, &keyErrorMessage)) {
        PrintNetworkKeyHash(networkKey);
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
        TrayIconMessageLoop();
    #elif defined(__APPLE__)
        std::thread signalThread([waitset]() mutable {
            int caughtSignal = 0;
            if (sigwait(&waitset, &caughtSignal) == 0) {
                RequestMacOSAppShutdown();
            }
        });
        signalThread.detach();
        RunMacOSStatusMenu();
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
