#include "Cli.h"

#include <CLI/CLI.hpp>

#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sodium.h>

#include "HostId.h"
#include "KeyManager.h"
#include "Logger.h"
#include "Settings.h"
#include "platform.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <shellapi.h>
    #include "platform/win32/CrashHandler.h"
#else
    #include <termios.h>
    #include <unistd.h>
#endif

// Globals owned elsewhere (declared extern in their headers).
//   g_settings   -> Settings.h
//   g_keyManager -> KeyManager.h
//   g_logger     -> Logger.h

bool InitializeSodium() {
    if (sodium_init() < 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Fatal: libsodium failed to initialize!");
        return false;
    }
    g_logger.log(__FUNCTION__, Logger::Level::Debug, "libsodium initialized successfully.");
    return true;
}

namespace {

// --- Output helpers ----------------------------------------------------------
// stdout carries command data/answers; stderr carries prompts and command-level
// errors. Diagnostic logs are a separate (gated) stderr stream via g_logger.
// On Windows we emit wide text to the console attached in InitializeConsoleOutput;
// on POSIX we emit raw UTF-8 bytes (locale-independent, and redirection-clean).

std::wstring ToWide(const std::string& utf8) {
    return clipp_platform_detail::Utf8ToUtf16String(utf8);
}

void OutLine(const std::wstring& line) {
#ifdef _WIN32
    std::wcout << line << L'\n';
#else
    std::cout << clipp_platform_detail::Utf16ToUtf8String(line) << '\n';
#endif
}

void ErrLine(const std::wstring& line) {
#ifdef _WIN32
    std::wcerr << line << L'\n';
#else
    std::cerr << clipp_platform_detail::Utf16ToUtf8String(line) << '\n';
#endif
}

void WritePrompt(const std::wstring& prompt) {
#ifdef _WIN32
    std::wcerr << prompt;
    std::wcerr.flush();
#else
    std::cerr << clipp_platform_detail::Utf16ToUtf8String(prompt);
    std::cerr.flush();
#endif
}

// --- Interactive input -------------------------------------------------------

bool StdinIsInteractive() {
#ifdef _WIN32
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD mode = 0;
    return GetConsoleMode(handle, &mode) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

std::string ReadLineVisible(const std::wstring& prompt) {
    WritePrompt(prompt);
    std::string input;
    std::getline(std::cin, input);
    return input;
}

std::string ReadHiddenLine(const std::wstring& prompt) {
    WritePrompt(prompt);
    std::string input;

#ifdef _WIN32
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
    std::getline(std::cin, input);
    SetConsoleMode(hStdin, mode);
#else
    termios oldt{};
    tcgetattr(STDIN_FILENO, &oldt);
    termios newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    std::getline(std::cin, input);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif

    // The user's Enter keystroke was not echoed; emit the newline ourselves.
    WritePrompt(L"\n");
    return input;
}

bool ParseLogLevel(std::string_view arg, Logger::Level& level) {
    if (arg == "error") { level = Logger::Level::Error; return true; }
    if (arg == "warn" || arg == "warning") { level = Logger::Level::Warning; return true; }
    if (arg == "info") { level = Logger::Level::Info; return true; }
    if (arg == "debug") { level = Logger::Level::Debug; return true; }
    if (arg == "ddebug") { level = Logger::Level::DDebug; return true; }
    return false;
}

// --- Subcommand handlers (return process exit codes) -------------------------

int RunKeySet(const std::string* nameOverride) {
    if (!StdinIsInteractive()) {
        ErrLine(L"`key set` needs an interactive console to read the secret.");
        return 1;
    }

    std::string networkName;
    if (nameOverride != nullptr) {
        networkName = *nameOverride;
    } else {
        networkName = ReadLineVisible(L"Enter network name: ");
    }
    if (networkName.empty()) {
        ErrLine(L"No network name provided.");
        return 1;
    }

    std::string secret = ReadHiddenLine(L"Enter a password to derive the network key from: ");
    if (secret.empty()) {
        ErrLine(L"No password provided.");
        return 1;
    }

    // Persist the raw name (canonicalization is derivation-only), matching the GUI.
    if (!g_settings.set_networkName(networkName)) {
        sodium_memzero(secret.data(), secret.capacity());
        ErrLine(L"Failed to store network name.");
        return 1;
    }

    std::string keyInput = KeyManager::BuildKeyDerivationInput(networkName, secret);
    sodium_memzero(secret.data(), secret.capacity());

    KeyManager::NetworkKey networkKey{};
    const bool derived = g_keyManager.DeriveNetworkKey(keyInput, networkKey);
    sodium_memzero(keyInput.data(), keyInput.capacity());
    if (!derived) {
        ErrLine(L"Failed to derive network key from password.");
        return 1;
    }

    std::string errorMessage;
    if (!g_keyManager.SetNetworkKey(networkKey, &errorMessage)) {
        sodium_memzero(networkKey.data(), networkKey.size());
        std::wstring message = L"Failed to store network key.";
        if (!errorMessage.empty()) {
            message += L" ";
            message += ToWide(errorMessage);
        }
        ErrLine(message);
        return 1;
    }

    const std::wstring fingerprint = g_keyManager.GetNetworkFingerprintHash(&networkKey);
    sodium_memzero(networkKey.data(), networkKey.size());

    OutLine(L"fingerprint: " + fingerprint);
    ErrLine(L"Network key saved.");
    return 0;
}

int RunKeyErase() {
    g_keyManager.ClearNetworkKey();
    ErrLine(L"Network key erased.");
    return 0;
}

int RunKeyShow() {
    const std::string networkName = g_settings.networkName();
    const std::wstring fingerprint = g_keyManager.GetNetworkFingerprintHash(nullptr);

    OutLine(L"name: " + ToWide(networkName));
    OutLine(L"fingerprint: " + (fingerprint.empty() ? std::wstring(L"(none)") : fingerprint));
    return 0;
}

int RunHostIdShow() {
    HostId hostID;
    if (!g_settings.getHostID(hostID)) {
        OutLine(L"hostid: (none)");
        return 0;
    }
    OutLine(L"hostid: " + hostID.ToHexWString());
    return 0;
}

int RunHostIdReset() {
    HostId hostID;
    if (!g_settings.resetHostID(hostID)) {
        ErrLine(L"Failed to reset host ID.");
        return 1;
    }
    OutLine(L"hostid: " + hostID.ToHexWString());
    ErrLine(L"Host ID reset.");
    return 0;
}

enum class Action { None, KeySet, KeyErase, KeyShow, HostIdShow, HostIdReset };

}  // namespace

namespace clipp::cli {

std::optional<int> Run(int argc, char** argv) {
#ifdef _WIN32
    // The CRT's narrow argv is ANSI-derived and mangles non-ASCII (e.g.
    // `--name "Büro"`). Rebuild a UTF-8 argv from the wide command line.
    std::vector<std::string> utf8Storage;
    std::vector<char*> utf8Argv;
    {
        int wideArgc = 0;
        LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
        if (wideArgv != nullptr) {
            utf8Storage.reserve(static_cast<size_t>(wideArgc));
            for (int i = 0; i < wideArgc; ++i) {
                utf8Storage.push_back(clipp_platform_detail::Utf16ToUtf8String(wideArgv[i]));
            }
            LocalFree(wideArgv);

            utf8Argv.reserve(utf8Storage.size() + 1);
            for (std::string& argument : utf8Storage) {
                utf8Argv.push_back(argument.data());
            }
            utf8Argv.push_back(nullptr);

            argc = static_cast<int>(utf8Storage.size());
            argv = utf8Argv.data();
        }
    }
#endif

    CLI::App app{"Clipp - cross-platform clipboard sharing"};
    app.allow_extras();   // a bare/GUI launch (and OS-injected args) must not error
    app.fallthrough();    // let --loglevel be recognized before or after a subcommand

    std::string logLevel;
    CLI::Option* logLevelOption = app.add_option(
        "--loglevel", logLevel,
        "Log level: error, warn, info, debug, ddebug (default: silent in command mode)");

    Action action = Action::None;

    CLI::App* keyCommand = app.add_subcommand("key", "Network key management");
    keyCommand->require_subcommand(1);

    std::string keySetName;
    CLI::App* keySet = keyCommand->add_subcommand("set", "Set the network name and derive the key from a secret");
    CLI::Option* keySetNameOption = keySet->add_option("--name", keySetName, "Network name (prompted if omitted)");
    keySet->callback([&]() { action = Action::KeySet; });

    CLI::App* keyErase = keyCommand->add_subcommand("erase", "Erase the stored network key");
    keyErase->callback([&]() { action = Action::KeyErase; });

    CLI::App* keyShow = keyCommand->add_subcommand("show", "Show the network name and key fingerprint");
    keyShow->callback([&]() { action = Action::KeyShow; });

    CLI::App* hostIdCommand = app.add_subcommand("hostid", "This device's host identity");
    hostIdCommand->require_subcommand(1);

    CLI::App* hostIdShow = hostIdCommand->add_subcommand("show", "Show this device's host ID");
    hostIdShow->callback([&]() { action = Action::HostIdShow; });

    CLI::App* hostIdReset = hostIdCommand->add_subcommand("reset", "Reset this device's host ID");
    hostIdReset->callback([&]() { action = Action::HostIdReset; });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        // Prints help (exit 0) or the error (nonzero) to the attached console.
        return app.exit(error);
    }

    // Apply the logging policy now that global options are parsed.
    if (logLevelOption->count() > 0) {
        Logger::Level level{};
        if (!ParseLogLevel(logLevel, level)) {
            ErrLine(L"Invalid --loglevel '" + ToWide(logLevel) +
                    L"'. Expected one of: error, warn, info, debug, ddebug.");
            return 1;
        }
        g_logger.SetMinimumLevel(level);
    } else if (action != Action::None) {
        g_logger.SetMinimumLevel(Logger::Level::Off);
    }

#ifdef _WIN32
    // Install the crash handler now that the log level is decided: its "installed"
    // line is suppressed in command mode (level Off) but still shown for the GUI.
    // Once installed it is process-global, so it guards both the command handlers
    // below and the GUI loop that main() runs after we return nullopt.
    clipp::InstallCrashHandler();
#endif

    if (action == Action::None) {
        return std::nullopt;  // no subcommand -> proceed to the GUI
    }

    if (!InitializeSodium()) {
        return 1;
    }

    switch (action) {
    case Action::KeySet:
        return RunKeySet(keySetNameOption->count() > 0 ? &keySetName : nullptr);
    case Action::KeyErase:
        return RunKeyErase();
    case Action::KeyShow:
        return RunKeyShow();
    case Action::HostIdShow:
        return RunHostIdShow();
    case Action::HostIdReset:
        return RunHostIdReset();
    case Action::None:
        break;
    }
    return std::nullopt;
}

}  // namespace clipp::cli
