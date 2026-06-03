#include "Cli.h"

#include <CLI/CLI.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sodium.h>

#include "ClipboardPayload.h"
#include "ClipboardWire.h"
#include "CryptoChannel.h"
#include "HostId.h"
#include "KeyManager.h"
#include "LocalPeerName.h"
#include "Logger.h"
#include "MDNSDiscovery.h"
#include "OneShotPeer.h"
#include "Settings.h"
#include "platform.h"
#include "platform/LogPaths.h"
#include "version.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <shellapi.h>
    #include <io.h>
    #include <fcntl.h>
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
// On a console we emit wide text (Windows renders it directly via the console
// API); when the stream is a redirected file or pipe we emit raw UTF-8 bytes --
// locale-independent and redirection-clean (and what binary `paste` will build on).

std::wstring ToWide(const std::string& utf8) {
    return clipp_platform_detail::Utf8ToUtf16String(utf8);
}

#ifdef _WIN32
bool StdHandleIsConsole(DWORD stdHandleId) {
    HANDLE handle = GetStdHandle(stdHandleId);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    return GetFileType(handle) == FILE_TYPE_CHAR;  // console (or other char device)
}
#endif

// stderr gets a built-in red on an interactive terminal — signed builds block
// DYLD-injection tools like stderred, so we color our own stderr. Plain on a pipe/file.
bool StderrIsTty() {
#ifdef _WIN32
    return StdHandleIsConsole(STD_ERROR_HANDLE);
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

void OutLine(const std::wstring& line) {
#ifdef _WIN32
    if (StdHandleIsConsole(STD_OUTPUT_HANDLE)) {
        std::wcout << line << L'\n';
        return;
    }
#endif
    std::cout << clipp_platform_detail::Utf16ToUtf8String(line) << '\n';
}

void ErrLine(const std::wstring& line) {
#ifdef _WIN32
    if (StdHandleIsConsole(STD_ERROR_HANDLE)) {
        std::wcerr << L"\x1b[0;91m" << line << L"\x1b[0m\n";
        return;
    }
#endif
    const std::string utf8 = clipp_platform_detail::Utf16ToUtf8String(line);
    if (StderrIsTty()) std::cerr << "\x1b[0;91m" << utf8 << "\x1b[0m\n";
    else               std::cerr << utf8 << '\n';
}

void WritePrompt(const std::wstring& prompt) {
    // Prompts stay default-colored — they're stderr, but a red prompt reads like an
    // error. Only diagnostics/verbose (ErrLine) and the logger get the stderr red.
#ifdef _WIN32
    if (StdHandleIsConsole(STD_ERROR_HANDLE)) {
        std::wcerr << prompt;
        std::wcerr.flush();
        return;
    }
#endif
    std::cerr << clipp_platform_detail::Utf16ToUtf8String(prompt);
    std::cerr.flush();
}

// Verbose (`-v`) progress goes to stderr, separate from the gated g_logger stream.
// stdout stays reserved for command data (e.g. `paste` bytes).
bool g_verbose = false;
void VerboseLine(const std::wstring& line) {
    if (g_verbose) {
        ErrLine(line);
    }
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

// --- Network verbs support ---------------------------------------------------
// main()'s WSAStartup runs only on the GUI path (after cli::Run returns nullopt),
// so the network verbs (copy/paste) must initialize winsock themselves. No-op
// shim off Windows.
#ifdef _WIN32
struct NetworkStartup {
    bool ok = false;
    NetworkStartup() { WSADATA data; ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0); }
    ~NetworkStartup() { if (ok) WSACleanup(); }
    NetworkStartup(const NetworkStartup&) = delete;
    NetworkStartup& operator=(const NetworkStartup&) = delete;
};
#else
struct NetworkStartup { bool ok = true; };
#endif

// Reads stdin to EOF, for `copy`.
std::vector<unsigned char> ReadAllStdin() {
#ifdef _WIN32
    // Console (interactive typing) needs text mode so a Ctrl+Z (0x1A) signals EOF and
    // typed CRLFs fold to LF; binary mode would read Ctrl+Z as a literal byte and the
    // read would never finish. Redirected stdin (pipe/file) uses binary mode for
    // byte-exact, translation-free input — its EOF comes from the stream closing.
    _setmode(_fileno(stdin), StdinIsInteractive() ? _O_TEXT : _O_BINARY);
#endif
    std::vector<unsigned char> data;
    unsigned char buffer[65536];
    size_t readCount = 0;
    while ((readCount = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        data.insert(data.end(), buffer, buffer + readCount);
    }
    return data;
}

// Writes bytes to stdout, binary-safe on Windows (no LF->CRLF translation), for
// `paste`.
void WriteAllStdout(const unsigned char* data, size_t size) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (size > 0) {
        std::fwrite(data, 1, size, stdout);
    }
    std::fflush(stdout);
}

// "<name> (<ip>)" with a "[this device]" tag for same-hostId peers, for -v output.
std::wstring PeerLabel(const MDNSDiscovery::DiscoveredPeer& peer, const HostId& localHostId) {
    std::wstring label = ToWide(peer.deviceName) + L" (" + ToWide(peer.ip) + L")";
    if (peer.hostId == localHostId) {
        label += L" [this device]";
    }
    return label;
}

// --- Subcommand handlers (return process exit codes) -------------------------

int RunKeySet(const std::string* nameOverride) {
    if (!StdinIsInteractive()) {
        ErrLine(L"`key set` needs an interactive console to read the passphrase.");
        return 1;
    }

    std::string networkName;
    if (nameOverride != nullptr) {
        networkName = *nameOverride;
    } else {
        networkName = ReadLineVisible(L"Enter group name: ");
    }
    if (networkName.empty()) {
        ErrLine(L"No group name provided.");
        return 1;
    }

    std::string secret = ReadHiddenLine(L"Enter a passphrase to derive the group key from: ");
    if (secret.empty()) {
        ErrLine(L"No passphrase provided.");
        return 1;
    }

    // Persist the raw name (canonicalization is derivation-only), matching the GUI.
    if (!g_settings.set_networkName(networkName)) {
        sodium_memzero(secret.data(), secret.capacity());
        ErrLine(L"Failed to store group name.");
        return 1;
    }

    std::string keyInput = KeyManager::BuildKeyDerivationInput(networkName, secret);
    sodium_memzero(secret.data(), secret.capacity());

    KeyManager::NetworkKey networkKey{};
    const bool derived = g_keyManager.DeriveNetworkKey(keyInput, networkKey);
    sodium_memzero(keyInput.data(), keyInput.capacity());
    if (!derived) {
        ErrLine(L"Failed to derive group key from passphrase.");
        return 1;
    }

    std::string errorMessage;
    if (!g_keyManager.SetNetworkKey(networkKey, &errorMessage)) {
        sodium_memzero(networkKey.data(), networkKey.size());
        std::wstring message = L"Failed to store group key.";
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
    ErrLine(L"Group key saved.");
    return 0;
}

int RunKeyErase() {
    g_keyManager.ClearNetworkKey();
    ErrLine(L"Group key erased.");
    return 0;
}

int RunKeyShow() {
    const std::string networkName = g_settings.networkName();
    OutLine(L"name: " + ToWide(networkName));

    std::string errorMessage;
    const std::wstring fingerprint = g_keyManager.GetNetworkFingerprintHash(nullptr, &errorMessage);
    if (!fingerprint.empty()) {
        OutLine(L"fingerprint: " + fingerprint);
        return 0;
    }

    OutLine(L"fingerprint: (none)");
    if (!errorMessage.empty()) {
        // Empty fingerprint *with* a reason means a key may well exist but couldn't
        // be read (keychain unavailable over SSH and the app isn't running to vend
        // it). Surface it loudly rather than implying there's no key. An empty
        // reason is the benign "no key configured" case -> exit 0.
        ErrLine(L"could not read the group key: " + ToWide(errorMessage));
        return 1;
    }
    return 0;
}

int RunHostIdShow() {
    // ensure, not get: a device's host ID is meant to exist from first use. On the
    // desktop builds main() generates it at startup; the headless CLI has no such
    // daemon, so create-and-persist it on first read here (idempotent thereafter).
    // This also fixes a latent desktop case -- `hostid show` before the GUI's first
    // launch previously printed "(none)".
    HostId hostID;
    if (!g_settings.ensureHostID(hostID)) {
        ErrLine(L"Failed to initialize host ID.");
        return 1;
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

// Reads stdin and pushes it to the network as a UTF-8 text item. Finds one gateway
// peer (the first that accepts) and relays through it — that peer rebroadcasts to the
// synced mesh, so one push reaches everyone. No GUI required on this machine.
int RunCopy() {
    if (!g_keyManager.HaveNetworkKey()) {
        ErrLine(L"No group key configured. Run `clipp key set` first.");
        return 1;
    }

    NetworkStartup net;
    if (!net.ok) {
        ErrLine(L"Failed to initialize networking.");
        return 1;
    }

    std::vector<unsigned char> bytes = ReadAllStdin();
    if (bytes.empty()) {
        ErrLine(L"Nothing on stdin to copy.");
        return 1;
    }
    VerboseLine(L"Read " + std::to_wstring(bytes.size()) + L" byte(s) from stdin.");

    // v1: UTF-8 text. Append a trailing NUL to match the platform capture
    // convention (receivers strip one trailing NUL when writing to the clipboard),
    // so CLI-copied text is byte-identical to GUI-copied text.
    bytes.push_back('\0');

    ClipboardPayload payload;
    payload.meta.formatId = CLIPP_FORMAT_UTF8;
    if (!payload.SetUncompressedBytes(std::move(bytes))) {
        ErrLine(L"Failed to encode clipboard payload.");
        return 1;
    }

    // ensure (not get): stamp the relay origin with a real host ID. A headless box
    // may never have run the daemon that generates it, and an all-zero id here would
    // poison origin/identity on the wire. Idempotent once it exists.
    HostId localHostId;
    g_settings.ensureHostID(localHostId);
    const std::string localHostName = clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);
    payload.StampOrigin(localHostId, localHostName.c_str(), g_settings.nextOriginSequenceNumber());

    std::vector<ClipboardPayload> payloads;
    payloads.push_back(std::move(payload));
    const auto via = OneShot::RelayPayloads(std::move(payloads), localHostId, localHostName, /*includeSelf=*/true);
    if (!via) {
        ErrLine(L"Could not reach any device to copy to.");
        return 1;
    }
    VerboseLine(L"Relayed via " + PeerLabel(*via, localHostId) + L".");
    return 0;
}

enum class FetchResult { Content, Empty, NoSupport, Failed };

// Runs the RCNT exchange on an already-connected peer. On a text CLIP response,
// writes the payload to stdout and returns Content. NONE / non-text -> Empty;
// missing cap -> NoSupport; transport/protocol failure -> Failed. Handles the SYNC
// crosstalk the GUI sends a fresh incoming peer.
FetchResult FetchRecent(OneShotPeer& connection) {
    if ((connection.RemoteCaps()[0] & CryptoChannel::CAP0_SERVES_RECENT) == 0) {
        return FetchResult::NoSupport;
    }
    if (!connection.SendFrame("RCNT")) {
        return FetchResult::Failed;
    }

    std::vector<unsigned char> frame;
    while (connection.RecvFrame(frame)) {
        if (frame.size() < 4) {
            break;
        }
        if (std::memcmp(frame.data(), "CLIP", 4) == 0) {
            ClipboardPayload payload;
            if (!ClipboardWire::TryDecodeClipboardFrame(frame, payload)) {
                return FetchResult::Failed;
            }
            // Text-only: a non-text newest item reads as "nothing" (the responder
            // gates on this too, but guard the requester side as well).
            if (payload.meta.formatId != CLIPP_FORMAT_UTF8) {
                return FetchResult::Empty;
            }
            // Localized form: native line endings for stdout (CRLF on Windows, LF
            // elsewhere) — the wire is LF-canonical.
            const std::vector<unsigned char>* plaintext = payload.TryGetLocalizedBytes();
            if (plaintext == nullptr) {
                return FetchResult::Failed;
            }
            size_t size = plaintext->size();
            // Text carries a trailing NUL on the wire (capture convention); strip one
            // so stdout matches pbpaste.
            if (size > 0 && plaintext->back() == '\0') {
                --size;
            }
            VerboseLine(L"  got " + std::to_wstring(size) + L" byte(s).");
            WriteAllStdout(plaintext->data(), size);
            return FetchResult::Content;
        }
        if (std::memcmp(frame.data(), "NONE", 4) == 0) {
            return FetchResult::Empty;
        }
        if (std::memcmp(frame.data(), "SYNC", 4) == 0) {
            if (!connection.SendFrame("EOSY")) {
                return FetchResult::Failed;
            }
            continue;
        }
        // Any other tag (e.g. a stray PING): ignore and keep reading for our reply.
    }
    return FetchResult::Failed;
}

// Fetches the newest clipboard item from a device and writes it to stdout. Streams
// discovery and tries each peer as it resolves, stopping at the first that serves
// text content — in the synced mesh, any peer's newest item is the network's newest,
// so there's no reason to wait for the full peer set. Text-only.
int RunPaste() {
    if (!g_keyManager.HaveNetworkKey()) {
        ErrLine(L"No group key configured. Run `clipp key set` first.");
        return 1;
    }

    NetworkStartup net;
    if (!net.ok) {
        ErrLine(L"Failed to initialize networking.");
        return 1;
    }

    // ensure (not get): the paste handshake authenticates with our host ID; a
    // headless box may not have generated one yet, and an all-zero id would
    // misidentify us to the peer. Idempotent once it exists.
    HostId localHostId;
    g_settings.ensureHostID(localHostId);
    const std::string localHostName = clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);

    const bool got = MDNSDiscovery::BrowseStream(OneShot::kBrowseCeiling, /*includeSelf=*/true,
        [&](const MDNSDiscovery::DiscoveredPeer& peer) -> bool {
            VerboseLine(L"Trying " + PeerLabel(peer, localHostId) + L"...");
            OneShotPeer connection;
            if (!connection.Connect(peer.ip, peer.port, localHostId, localHostName, peer.hostId,
                                    OneShot::kConnectTimeout, OneShot::kSessionTimeout)) {
                VerboseLine(L"  unreachable.");
                return true;  // try the next peer
            }
            switch (FetchRecent(connection)) {  // writes stdout on Content
            case FetchResult::Content:   return false;  // got it; stop browsing
            case FetchResult::Empty:     VerboseLine(L"  no content."); return true;
            case FetchResult::NoSupport: VerboseLine(L"  no paste support, skipping."); return true;
            case FetchResult::Failed:    VerboseLine(L"  request failed."); return true;
            }
            return true;
        });

    if (!got) {
        ErrLine(L"No clipboard content available from any device.");
        return 1;
    }
    return 0;
}

enum class Action { None, Gui, KeySet, KeyErase, KeyShow, HostIdShow, HostIdReset, Copy, Paste };

}  // namespace

namespace clipp::cli {

std::optional<int> Run(int argc, char** argv, bool launchedFromConsole) {
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

    CLI::App app{"Clipp v" CLIPP_VERSION_STRING_3PART " - cross-platform clipboard sharing"};
    app.allow_extras();   // a bare/GUI launch (and OS-injected args) must not error
    app.fallthrough();    // let --loglevel be recognized before or after a subcommand

    std::string logLevel;
    CLI::Option* logLevelOption = app.add_option(
        "--loglevel", logLevel,
        "Log level: error, warn, info, debug, ddebug (default: silent in command mode)");

    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Print progress to stderr (for copy/paste)");

    Action action = Action::None;

    CLI::App* copyCommand = app.add_subcommand("copy", "Read stdin and copy it to the network");
    copyCommand->alias("c");
    copyCommand->callback([&]() { action = Action::Copy; });

    CLI::App* pasteCommand = app.add_subcommand("paste", "Fetch the newest clipboard item from the network and write it to stdout");
    pasteCommand->alias("p");
    pasteCommand->callback([&]() { action = Action::Paste; });

    CLI::App* keyCommand = app.add_subcommand("key", "Group key management");
    keyCommand->require_subcommand(1);

    std::string keySetName;
    CLI::App* keySet = keyCommand->add_subcommand("set", "Set the group name and derive the key from a passphrase");
    CLI::Option* keySetNameOption = keySet->add_option("--name", keySetName, "Group name (prompted if omitted)");
    keySet->callback([&]() { action = Action::KeySet; });

    CLI::App* keyErase = keyCommand->add_subcommand("erase", "Erase the stored group key");
    keyErase->callback([&]() { action = Action::KeyErase; });

    CLI::App* keyShow = keyCommand->add_subcommand("show", "Show the group name and key fingerprint");
    keyShow->callback([&]() { action = Action::KeyShow; });

    CLI::App* hostIdCommand = app.add_subcommand("hostid", "This device's host identity");
    hostIdCommand->require_subcommand(1);

    CLI::App* hostIdShow = hostIdCommand->add_subcommand("show", "Show this device's host ID");
    hostIdShow->callback([&]() { action = Action::HostIdShow; });

    CLI::App* hostIdReset = hostIdCommand->add_subcommand("reset", "Reset this device's host ID");
    hostIdReset->callback([&]() { action = Action::HostIdReset; });

#ifndef CLIPP_HEADLESS
    // Explicit opt-in to the GUI from a terminal (a bare launch there prints usage
    // instead). Listed in --help but not advertised harder than that. Omitted from
    // the terminal-only build, which has no GUI to launch.
    CLI::App* guiCommand = app.add_subcommand(
        "gui", "Launch the graphical app (the default when not started from a terminal)");
    guiCommand->callback([&]() { action = Action::Gui; });
#endif

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        // Prints help (exit 0) or the error (nonzero) to the attached console.
        return app.exit(error);
    }

    // A bare launch and the explicit `gui` subcommand are both GUI dispositions,
    // not commands -- they must not silence the logger the way a command does.
    const bool guiDisposition = (action == Action::None || action == Action::Gui);

    // Apply the logging policy now that global options are parsed.
    if (logLevelOption->count() > 0) {
        Logger::Level level{};
        if (!ParseLogLevel(logLevel, level)) {
            ErrLine(L"Invalid --loglevel '" + ToWide(logLevel) +
                    L"'. Expected one of: error, warn, info, debug, ddebug.");
            return 1;
        }
        g_logger.SetMinimumLevel(level);
    } else if (!guiDisposition) {
        g_logger.SetMinimumLevel(Logger::Level::Off);
    }

    g_verbose = verbose;

    // Bare launch (no command): print usage and exit instead of silently launching
    // the terminal-blocking GUI. On the desktop builds this is gated on a console (a
    // no-console launch falls through to the GUI via the `gui`/nullopt path below);
    // the headless build has no GUI, so a bare launch ALWAYS prints help.
#ifdef CLIPP_HEADLESS
    const bool bareLaunchPrintsHelp = (action == Action::None);
#else
    const bool bareLaunchPrintsHelp = (action == Action::None && launchedFromConsole);
#endif
    if (bareLaunchPrintsHelp) {
        g_logger.SetMinimumLevel(Logger::Level::Off);  // keep diagnostics out of the usage text
        std::cout << app.help();
        std::cout.flush();
        return 0;
    }

#if defined(_WIN32) || defined(__APPLE__)
    // Persist this launch's logs to a rolling per-launch file (and prune the
    // retention window) now that the level is decided and the usage early-return
    // above has been passed, so a bare `--help` launch writes nothing. The file is
    // lazy-opened on the first emitted line, so a silent command (level Off) leaves
    // no file behind. Desktop only -- the Linux CLI logs to stderr by design and iOS
    // file logs are inaccessible without archaeology.
    {
        std::string logDir;
        if (clipp::ResolveLogDirectory(logDir)) {
            g_logger.EnableFileLogging(logDir);
        }
    }
#endif

#ifdef _WIN32
    // Install the crash handler now that the log level is decided: its "installed"
    // line is suppressed in command mode (level Off) but still shown for the GUI.
    // Once installed it is process-global, so it guards both the command handlers
    // below and the GUI loop that main() runs after we return nullopt. (The usage
    // path above returns before here -- it has nothing to guard.)
    clipp::InstallCrashHandler();
#endif

    if (guiDisposition) {
        return std::nullopt;  // explicit `gui`, or a bare launch with no console -> GUI
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
    case Action::Copy:
        return RunCopy();
    case Action::Paste:
        return RunPaste();
    case Action::Gui:
    case Action::None:
        break;
    }
    return std::nullopt;
}

}  // namespace clipp::cli
