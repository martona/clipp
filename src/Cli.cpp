#include "Cli.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
#include "NetworkDefs.h"
#include "OneShotPeer.h"
#include "RegisterStore.h"
#include "RegisterWire.h"
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
    #include <sys/ioctl.h>
    #include <sys/stat.h>
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

// --- Stream dispositions -------------------------------------------------------
// What each standard stream is actually connected to. Drives tty-styling and the
// bare-launch verb inference in Run(). Four-way on purpose: only a real pipe or
// regular file may infer a verb — a terminal is interactive, and Absent (no handle,
// NUL, /dev/null, sockets) covers the launch contexts (Task Scheduler, cron,
// launchd, Dock) where inferring `copy` would block on stdin or clobber the
// group clipboard.

enum class StreamDisposition { Terminal, Pipe, File, Absent };

#ifdef _WIN32
// mintty (the Git Bash / MSYS2 / Cygwin default terminal) is not a Windows console:
// programs it runs get MSYS pty PIPES as their standard handles, so an interactive
// prompt there is indistinguishable from redirection by handle type alone. The pty
// pipe names look like \msys-<hex>-pty0-from-master / \cygwin-<hex>-pty1-to-master
// (bash's own `|` pipes are named ...-pipe-..., so they don't match); sniffing that
// shape is the standard trick (Rust is-terminal, Node) to classify an interactive
// mintty stream as a terminal rather than a redirect.
bool PipeIsMsysPty(HANDLE handle) {
    struct {
        FILE_NAME_INFO info;
        wchar_t namePadding[MAX_PATH];  // room for FILE_NAME_INFO's flexible array
    } buffer{};
    if (!GetFileInformationByHandleEx(handle, FileNameInfo, &buffer, sizeof(buffer))) {
        return false;  // anonymous pipe (no name) or name too long: a real redirect
    }
    const std::wstring name(buffer.info.FileName, buffer.info.FileNameLength / sizeof(wchar_t));
    if (name.find(L"msys-") == std::wstring::npos && name.find(L"cygwin-") == std::wstring::npos) {
        return false;
    }
    return name.find(L"-pty") != std::wstring::npos && name.find(L"-master") != std::wstring::npos;
}

// Runs after main()'s InitializeConsoleOutput(), and depends on it: interactive
// streams under the clipp.com shim arrive as console handles a GUI-subsystem
// process can't use, and only classify as Terminal because init re-attached the
// parent console and reopened them. Redirected pipe/file handles are left in place
// by init (their CRT fds bound), so they classify as-is.
StreamDisposition ClassifyStdStream(DWORD stdHandleId) {
    HANDLE handle = GetStdHandle(stdHandleId);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return StreamDisposition::Absent;
    }
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode) != 0) {
        return StreamDisposition::Terminal;  // a real console; NUL is CHAR but fails this
    }
    switch (GetFileType(handle)) {
    case FILE_TYPE_DISK: return StreamDisposition::File;
    case FILE_TYPE_PIPE:  // includes sockets
        return PipeIsMsysPty(handle) ? StreamDisposition::Terminal : StreamDisposition::Pipe;
    default:             return StreamDisposition::Absent;  // NUL, unknown
    }
}
StreamDisposition StdinDisposition()  { return ClassifyStdStream(STD_INPUT_HANDLE); }
StreamDisposition StdoutDisposition() { return ClassifyStdStream(STD_OUTPUT_HANDLE); }
StreamDisposition StderrDisposition() { return ClassifyStdStream(STD_ERROR_HANDLE); }
#else
StreamDisposition ClassifyStdStream(int fd) {
    if (isatty(fd) != 0) {
        return StreamDisposition::Terminal;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        return StreamDisposition::Absent;  // closed / bad fd
    }
    if (S_ISFIFO(st.st_mode)) return StreamDisposition::Pipe;
    if (S_ISREG(st.st_mode))  return StreamDisposition::File;
    // Char devices (/dev/null), sockets, directories. Sockets deliberately do NOT
    // count as Pipe: `ssh host clipp` on a socketpair-based sshd then prints usage
    // instead of silently blocking on an inferred copy. (Pipe-based sshds still
    // block; that footgun is inherent — use an explicit verb, or ssh -n.)
    return StreamDisposition::Absent;
}
StreamDisposition StdinDisposition()  { return ClassifyStdStream(STDIN_FILENO); }
StreamDisposition StdoutDisposition() { return ClassifyStdStream(STDOUT_FILENO); }
StreamDisposition StderrDisposition() { return ClassifyStdStream(STDERR_FILENO); }
#endif

// stderr gets a built-in red on an interactive terminal — signed builds block
// DYLD-injection tools like stderred, so we color our own stderr. Plain on a pipe/file.
bool StderrIsTty() {
    return StderrDisposition() == StreamDisposition::Terminal;
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
// --host filter: when non-empty, network verbs talk only to the device whose name
// (case-insensitive) or IP equals this. Empty = connect to the first peer found.
std::string g_hostFilter;
void VerboseLine(const std::wstring& line) {
    if (g_verbose) {
        ErrLine(line);
    }
}

// --- Interactive input -------------------------------------------------------

// True only for a REAL console. Deliberately narrower than StdinDisposition():
// the console-specific behaviors keyed off this (text-mode Ctrl+Z EOF in
// ReadAllStdin, echo suppression in ReadHiddenLine) don't apply to an msys pty,
// which wants the pipe treatment even though it classifies as Terminal.
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

// Failure message for the network verbs. The frequent mistake is handing --host a
// hostname / FQDN / "localhost": --host is an EXACT name-or-IP filter over discovered
// peers, NOT a resolver, so those silently match nothing and collapse to the same
// generic failure. When a filter is set and nothing matched, say so and point at the
// command that lists the valid values; otherwise emit the verb's own default message
// (which also covers the matched-but-unreachable / no-content cases).
void ReportNoGateway(const std::wstring& defaultMessage, bool filterMatched) {
    if (!g_hostFilter.empty() && !filterMatched) {
        ErrLine(L"No discovered device matched --host '" + ToWide(g_hostFilter) +
                L"'. --host is an exact device-name or IP filter, not a hostname lookup — "
                L"run `clipp peers` to see the names and IPs to use.");
    } else {
        ErrLine(defaultMessage);
    }
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
// echoStdinToStdout is the inferred `x | clipp | y` mode: tee the consumed bytes
// back out (explicit `clipp copy` never echoes).
int RunCopy(bool echoStdinToStdout) {
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

    // Tee mode keeps its own copy: `bytes` is consumed by the payload below.
    std::vector<unsigned char> echoBytes;
    if (echoStdinToStdout) {
        echoBytes = bytes;
    }

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
    bool filterMatched = false;
    const auto via = OneShot::RelayPayloads(std::move(payloads), localHostId, localHostName,
                                            /*includeSelf=*/true, g_hostFilter, &filterMatched);
    int exitCode = 0;
    if (!via) {
        ReportNoGateway(L"Could not reach any device to copy to.", filterMatched);
        exitCode = 1;
    } else {
        VerboseLine(L"Relayed via " + PeerLabel(*via, localHostId) + L".");
    }

    // The tee happens even when the relay failed — clipp sits mid-pipeline, and
    // eating the stream would turn a network error into downstream data loss — but
    // only AFTER the relay: a consumer like `head -1` may close the pipe and
    // SIGPIPE-kill us on this write, and the copy must have landed by then. This is
    // also the inferred both-ways "paste" leg: the bytes are already in hand, so it
    // never re-reads the network.
    if (echoStdinToStdout) {
        WriteAllStdout(echoBytes.data(), echoBytes.size());
    }
    return exitCode;
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

    bool filterMatched = false;
    const bool got = MDNSDiscovery::BrowseStream(OneShot::kBrowseCeiling, /*includeSelf=*/true,
        [&](const MDNSDiscovery::DiscoveredPeer& peer) -> bool {
            if (!g_hostFilter.empty() && !OneShot::PeerMatchesHost(peer, g_hostFilter)) return true;  // --host: skip other devices
            filterMatched = true;
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
        ReportNoGateway(L"No clipboard content available from any device.", filterMatched);
        return 1;
    }
    return 0;
}

// ---- Named registers: clipp copy/paste/ls/rm <name> -------------------------

bool StdoutIsTty() {
    return StdoutDisposition() == StreamDisposition::Terminal;
}

std::wstring FormatAgeSeconds(uint64_t s) {
    if (s < 60)    return std::to_wstring(s) + L"s";
    if (s < 3600)  return std::to_wstring(s / 60) + L"m";
    if (s < 86400) return std::to_wstring(s / 3600) + L"h";
    return std::to_wstring(s / 86400) + L"d";
}

std::wstring FormatAge(const Hlc& touched) {
    const uint64_t now = HlcClock::SystemNowMs();
    const uint64_t ageMs = (now > touched.wallMs) ? (now - touched.wallMs) : 0;
    return FormatAgeSeconds(ageMs / 1000);
}

bool HasWildcard(const std::string& s) {
    return s.find_first_of("*?") != std::string::npos;
}

// Shell-style glob over ASCII names: '?' = any one char, '*' = any run (incl empty).
bool GlobMatch(const std::string& pat, const std::string& str) {
    size_t p = 0, s = 0, star = std::string::npos, ss = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) { ++p; ++s; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; ss = s; }
        else if (star != std::string::npos) { p = star + 1; s = ++ss; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::wstring HumanizeSize(uint64_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes);
    if (bytes < 1024 * 1024) return std::to_wstring(bytes / 1024) + L"K";
    return std::to_wstring(bytes / (1024 * 1024)) + L"M";
}

// The CLI has no hostId->hostname map (that lives in the GUI), so show a short
// hostId prefix for the origin column.
std::wstring SanitizePreview(const std::string& preview) {
    std::string s = preview;
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = '.';   // control chars (incl newlines/tabs) -> dot
    }
    return ToWide(s);
}

// Origin column for `ls -v`: the server-resolved device name when present, else the
// short hostId prefix. The name is a peer-controlled field, so run it through the
// same control-char scrub as the preview to keep escape sequences out of the terminal.
std::wstring OriginLabel(const RegisterWire::RegisterListEntry& e) {
    if (!e.originHostName.empty()) return SanitizePreview(e.originHostName);
    return e.originHostId.ToHexWString(HostId::kSize).substr(0, 8);
}

int TerminalWidth() {
    if (const char* cols = std::getenv("COLUMNS")) {
        const int w = std::atoi(cols);
        if (w > 0) return w;
    }
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        const int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (w > 0) return w;
    }
#else
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return static_cast<int>(ws.ws_col);
    }
#endif
    return 80;
}

std::wstring PadRight(const std::wstring& s, size_t w) {
    return s.size() >= w ? s : s + std::wstring(w - s.size(), L' ');
}
std::wstring PadLeft(const std::wstring& s, size_t w) {
    return s.size() >= w ? s : std::wstring(w - s.size(), L' ') + s;
}

// Contents label for a binary register in `ls -v`, parsed from the preview's
// typed header (new daemons ship exactly the header as the preview; old daemons
// ship the first 256 value bytes, which begin with the same header).
std::wstring BinaryContentsLabel(const RegisterWire::RegisterListEntry& e) {
    RegisterWire::BinaryValueInfo info{};
    std::wstring kind = L"binary";
    uint64_t streamSize = e.valueSize;
    if (RegisterWire::TryParseBinaryValue(e.preview, info)) {
        if (info.formatId == CLIPP_FORMAT_PNG) kind = L"image/png";
        else if (info.formatId == CLIPP_FORMAT_JPEG) kind = L"image/jpeg";
        if (e.valueSize >= info.streamOffset) streamSize = e.valueSize - info.streamOffset;
    }
    return L"[" + kind + L", " + HumanizeSize(streamSize) + L"]";
}

// `ls -v`: an aligned name / age / size / origin / contents table, contents
// sanitized + width-capped with an overflow marker on a tty; private masked.
void FormatListVerbose(const std::vector<const RegisterWire::RegisterListEntry*>& entries) {
    size_t wName = 0, wAge = 0, wSize = 0, wOrigin = 0;
    for (const auto* e : entries) {
        const std::wstring dn = e->name.empty() ? L"(clipboard)" : ToWide(e->name);
        wName = (std::max)(wName, dn.size());
        wAge = (std::max)(wAge, FormatAge(e->touched).size());
        wSize = (std::max)(wSize, HumanizeSize(e->valueSize).size());
        wOrigin = (std::max)(wOrigin, OriginLabel(*e).size());
    }
    const int termW = TerminalWidth();
    const bool tty = StdoutIsTty();
    for (const auto* e : entries) {
        const std::wstring dn = e->name.empty() ? L"(clipboard)" : ToWide(e->name);
        std::wstring line = PadRight(dn, wName) + L"  " + PadLeft(FormatAge(e->touched), wAge) + L"  " +
                            PadLeft(HumanizeSize(e->valueSize), wSize) + L"  " +
                            PadRight(OriginLabel(*e), wOrigin) + L"  ";
        std::wstring contents =
            (e->flags & RegisterFlags::Private)      ? L"[private]"
            : (e->flags & RegisterFlags::BinaryHeader) ? BinaryContentsLabel(*e)
                                                       : SanitizePreview(e->preview);
        int remain = termW - static_cast<int>(line.size());
        if (remain < 4) remain = 4;
        bool overflow = false;
        if (static_cast<int>(contents.size()) > remain) {
            contents = contents.substr(0, static_cast<size_t>(remain) - 1);
            overflow = true;
        }
        line += contents;
        if (overflow) line += tty ? L"\x1b[7m>\x1b[0m" : L">";
        OutLine(line);
    }
}

// Browse for a peer advertising CAP0_SERVES_REGISTERS, connect, and run `exchange`
// on it; stop at the first peer where `exchange` returns true (handled). Returns
// true if some register-capable peer handled it. Mirrors RunPaste's browse loop.
// `filterMatched` is set true if any discovered peer passed the --host filter, so the
// caller can tell a no-match (bad --host) from a genuine no-reachable-gateway.
template <typename Fn>
bool WithRegisterGateway(const HostId& localHostId, const std::string& localHostName,
                         bool& filterMatched, Fn exchange) {
    return MDNSDiscovery::BrowseStream(OneShot::kBrowseCeiling, /*includeSelf=*/true,
        [&](const MDNSDiscovery::DiscoveredPeer& peer) -> bool {
            if (!g_hostFilter.empty() && !OneShot::PeerMatchesHost(peer, g_hostFilter)) return true;  // --host: skip other devices
            filterMatched = true;
            VerboseLine(L"Trying " + PeerLabel(peer, localHostId) + L"...");
            OneShotPeer connection;
            if (!connection.Connect(peer.ip, peer.port, localHostId, localHostName, peer.hostId,
                                    OneShot::kConnectTimeout, OneShot::kSessionTimeout)) {
                VerboseLine(L"  unreachable.");
                return true;  // browse: try the next peer
            }
            if ((connection.RemoteCaps()[0] & CryptoChannel::CAP0_SERVES_REGISTERS) == 0) {
                VerboseLine(L"  no register support, skipping.");
                return true;
            }
            return !exchange(connection);  // handled -> return false to stop browsing
        });
}

int RunRegisterCopy(const std::string& nameArg, bool isPrivate) {
    // NFC at ingress: macOS input methods can produce decomposed sequences; the
    // same visual name must address the same register from every platform.
    const std::string name = clipp_platform_detail::NormalizeUtf8Canonical(nameArg);
    if (!IsValidRegisterName(name)) {
        ErrLine(L"Invalid register name: up to 64 bytes of printable UTF-8; '?' '*' '/' are reserved; no leading/trailing space.");
        return 1;
    }
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
    // Registers are text: fold CRLF/CR -> LF so the same content stored from any platform
    // or shell settles to one canonical form, matching clipboard text (SetUncompressedBytes).
    CanonicalizeCrlfToLf(bytes);

    HostId localHostId;
    g_settings.ensureHostID(localHostId);
    const std::string localHostName =
        clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);

    RegisterRecord rec;
    rec.name = name;
    rec.value.assign(bytes.begin(), bytes.end());
    rec.originHostId = localHostId;
    rec.flags = isPrivate ? RegisterFlags::Private : 0;
    // HLCs left zero: the gateway re-stamps from its authoritative clock (keeping our
    // origin). We wait for its ack so the socket stays open until the write lands —
    // a fire-and-forget close races the gateway's read and drops the frame.
    const std::vector<unsigned char> regw =
        RegisterWire::EncodeRecord(rec, RegisterWire::Transport::Relay);

    bool ok = false;
    bool refused = false;
    bool filterMatched = false;
    const bool reached = WithRegisterGateway(localHostId, localHostName, filterMatched, [&](OneShotPeer& conn) -> bool {
        if (!conn.SendFrame("REGW", regw.data(), static_cast<uint32_t>(regw.size()))) {
            return false;
        }
        std::vector<unsigned char> frame;
        while (conn.RecvFrame(frame)) {
            if (frame.size() < 4) {
                break;
            }
            if (std::memcmp(frame.data(), "ROKW", 4) == 0) { ok = true; return true; }
            if (std::memcmp(frame.data(), "RERR", 4) == 0) { refused = true; return true; }
            if (std::memcmp(frame.data(), "SYNC", 4) == 0) {
                conn.SendFrame("EOSY");
                continue;
            }
            // ignore RSYN/REGW/REOS/PING crosstalk; keep waiting for the ack
        }
        return false;
    });
    if (!reached) {
        ReportNoGateway(L"Could not reach any register-capable device.", filterMatched);
        return 1;
    }
    if (refused) {
        ErrLine(L"Register write refused (name, size, or count limit).");
        return 1;
    }
    if (!ok) {
        ErrLine(L"No acknowledgement from the gateway; the register was not stored.");
        return 1;
    }
    VerboseLine(L"Copied " + std::to_wstring(bytes.size()) + L" byte(s) to register '" +
                ToWide(name) + L"'.");
    return 0;
}

int RunRegisterPaste(const std::string& nameArg) {
    const std::string name = clipp_platform_detail::NormalizeUtf8Canonical(nameArg);
    if (!IsValidRegisterName(name)) {
        ErrLine(L"Invalid register name: up to 64 bytes of printable UTF-8; '?' '*' '/' are reserved; no leading/trailing space.");
        return 1;
    }
    if (!g_keyManager.HaveNetworkKey()) {
        ErrLine(L"No group key configured. Run `clipp key set` first.");
        return 1;
    }
    NetworkStartup net;
    if (!net.ok) {
        ErrLine(L"Failed to initialize networking.");
        return 1;
    }
    HostId localHostId;
    g_settings.ensureHostID(localHostId);
    const std::string localHostName =
        clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);
    const std::vector<unsigned char> req = RegisterWire::EncodeName(name);

    bool present = false;
    bool refusedTty = false;
    bool filterMatched = false;
    const bool reached = WithRegisterGateway(localHostId, localHostName, filterMatched, [&](OneShotPeer& conn) -> bool {
        if (!conn.SendFrame("RGET", req.data(), static_cast<uint32_t>(req.size()))) {
            return false;  // transport failed; try the next gateway
        }
        std::vector<unsigned char> frame;
        while (conn.RecvFrame(frame)) {
            if (frame.size() < 4) {
                break;
            }
            if (std::memcmp(frame.data(), "REGW", 4) == 0) {
                RegisterRecord rec;
                uint8_t transport = 0;
                const std::vector<unsigned char> fbody(frame.begin() + 4, frame.end());
                if (!RegisterWire::TryDecodeRecord(fbody, rec, transport)) {
                    return false;
                }
                if (rec.name != name) {
                    continue;  // an unsolicited broadcast for another register; keep waiting
                }
                present = true;
                RegisterWire::BinaryValueInfo binInfo{};
                const bool isBinary = rec.IsBinary();
                if (isBinary && !RegisterWire::TryParseBinaryValue(rec.value, binInfo)) {
                    binInfo.streamOffset = 0;  // malformed header: emit the whole value
                }
                if (rec.IsPrivate() && StdoutIsTty()) {
                    ErrLine(L"Register '" + ToWide(name) +
                            L"' is private; refusing to print to a terminal. Pipe it to read.");
                    refusedTty = true;
                } else if (isBinary && StdoutIsTty()) {
                    ErrLine(L"Register '" + ToWide(name) +
                            L"' holds binary data; refusing to print to a terminal. Pipe or redirect it.");
                    refusedTty = true;
                } else if (isBinary) {
                    // Header stripped, stream bytes verbatim — line-ending
                    // expansion is a text-only convention.
                    const size_t off = binInfo.streamOffset <= rec.value.size()
                        ? binInfo.streamOffset : 0;
                    WriteAllStdout(reinterpret_cast<const unsigned char*>(rec.value.data()) + off,
                                   rec.value.size() - off);
                } else {
#ifdef _WIN32
                    // Registers store canonical LF; expand to the native ending on egress
                    // (CRLF on Windows), mirroring clipboard paste (TryGetLocalizedBytes).
                    std::vector<unsigned char> out(rec.value.begin(), rec.value.end());
                    ExpandLfToCrlf(out);
                    WriteAllStdout(out.data(), out.size());
#else
                    WriteAllStdout(reinterpret_cast<const unsigned char*>(rec.value.data()),
                                   rec.value.size());
#endif
                }
                return true;
            }
            if (std::memcmp(frame.data(), "NONE", 4) == 0) {
                return true;  // definitive: no such register
            }
            if (std::memcmp(frame.data(), "SYNC", 4) == 0) {
                conn.SendFrame("EOSY");  // clipboard-sync crosstalk from the gateway
                continue;
            }
            // Ignore RSYN/REOS/PING and any other crosstalk; keep reading for our reply.
        }
        return false;
    });
    if (!reached) {
        ReportNoGateway(L"Could not reach any register-capable device.", filterMatched);
        return 1;
    }
    if (!present) {
        ErrLine(L"No such register '" + ToWide(name) + L"'.");
        return 1;
    }
    return refusedTty ? 1 : 0;
}

// Promotes a named register to the live network clipboard — `clipp put <name>` is
// the one-step form of `clipp paste <name> | clipp copy`. One uniform path for
// every gateway: RGET the record here, then push it back as a relay CLIP built
// with the right format (text = canonical LF + trailing NUL; binary = the raw
// stream with the header's CLIPP_FORMAT_*), PING/PONG-fenced so our close can't
// outrun the gateway's read (see OneShot::RelayPayloads). RPUT is no longer
// requested by this CLI: a binary register needs the record inspected
// client-side anyway, and no capability distinguishes a header-aware gateway
// from an older SERVES_PUT one that would relay a binary value as garbage
// text. Gateways keep serving RPUT (header-aware now) for older CLIs.
int RunPut(const std::string& nameArg) {
    const std::string name = clipp_platform_detail::NormalizeUtf8Canonical(nameArg);
    if (!IsValidRegisterName(name)) {
        ErrLine(L"Invalid register name: up to 64 bytes of printable UTF-8; '?' '*' '/' are reserved; no leading/trailing space.");
        return 1;
    }
    if (!g_keyManager.HaveNetworkKey()) {
        ErrLine(L"No group key configured. Run `clipp key set` first.");
        return 1;
    }
    NetworkStartup net;
    if (!net.ok) {
        ErrLine(L"Failed to initialize networking.");
        return 1;
    }
    HostId localHostId;
    g_settings.ensureHostID(localHostId);
    const std::string localHostName =
        clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);
    const std::vector<unsigned char> req = RegisterWire::EncodeName(name);

    bool promoted = false;
    bool absent = false;
    bool malformed = false;
    bool filterMatched = false;
    const bool reached = WithRegisterGateway(localHostId, localHostName, filterMatched, [&](OneShotPeer& conn) -> bool {
        // Fetch the record, then hand it back as the clipboard.
        VerboseLine(L"  running get -> relay.");
        if (!conn.SendFrame("RGET", req.data(), static_cast<uint32_t>(req.size()))) {
            return false;
        }
        std::optional<RegisterRecord> rec;
        {
            std::vector<unsigned char> frame;
            while (conn.RecvFrame(frame)) {
                if (frame.size() < 4) {
                    break;
                }
                if (std::memcmp(frame.data(), "REGW", 4) == 0) {
                    RegisterRecord candidate;
                    uint8_t transport = 0;
                    const std::vector<unsigned char> fbody(frame.begin() + 4, frame.end());
                    if (!RegisterWire::TryDecodeRecord(fbody, candidate, transport)) {
                        return false;
                    }
                    if (candidate.name != name) {
                        continue;  // an unsolicited broadcast for another register; keep waiting
                    }
                    rec = std::move(candidate);
                    break;
                }
                if (std::memcmp(frame.data(), "NONE", 4) == 0) { absent = true; return true; }
                if (std::memcmp(frame.data(), "SYNC", 4) == 0) {
                    conn.SendFrame("EOSY");
                    continue;
                }
                // Ignore RSYN/REOS/PING and any other crosstalk; keep waiting.
            }
            if (!rec.has_value()) {
                return false;  // transport died before an answer; try the next gateway
            }
        }

        // The same item `clipp copy` would send — canonical-LF text plus the
        // capture-convention trailing NUL — or, for a binary register, the raw
        // stream with the header's format. Relayed for mesh-wide fan-out.
        ClipboardPayload payload;
        std::vector<unsigned char> bytes;
        if (rec->IsBinary()) {
            RegisterWire::BinaryValueInfo info{};
            if (!RegisterWire::TryParseBinaryValue(rec->value, info)) {
                malformed = true;  // flagged but unparseable: never relay garbage
                return true;
            }
            payload.meta.formatId = info.formatId;
            bytes.assign(rec->value.begin() + static_cast<std::ptrdiff_t>(info.streamOffset),
                         rec->value.end());
        } else {
            payload.meta.formatId = CLIPP_FORMAT_UTF8;
            bytes.assign(rec->value.begin(), rec->value.end());
            bytes.push_back('\0');
        }
        if (!payload.SetUncompressedBytes(std::move(bytes))) {
            return false;
        }
        payload.StampOrigin(localHostId, localHostName.c_str(), g_settings.nextOriginSequenceNumber());
        payload.meta.flags |= NetworkDefs::CLPM_FLAG_RELAY;
        if (!conn.SendClipboardPayload(payload)) {
            return false;
        }
        // Same delivery fence as RelayPayloads: the gateway's serial recv loop means
        // a PONG proves the CLIP was consumed before we close.
        if (!conn.SendFrame("PING")) {
            return false;
        }
        std::vector<unsigned char> frame;
        while (conn.RecvFrame(frame)) {
            if (frame.size() < 4) {
                break;
            }
            if (std::memcmp(frame.data(), "PONG", 4) == 0) { promoted = true; return true; }
            if (std::memcmp(frame.data(), "SYNC", 4) == 0) {
                if (!conn.SendFrame("EOSY")) {
                    break;
                }
                continue;
            }
            // Ignore RSYN/REGW/CLIP crosstalk while waiting for the fence.
        }
        return false;
    });

    if (!reached) {
        ReportNoGateway(L"Could not reach any register-capable device.", filterMatched);
        return 1;
    }
    if (absent) {
        ErrLine(L"No such register '" + ToWide(name) + L"'.");
        return 1;
    }
    if (malformed) {
        ErrLine(L"Register '" + ToWide(name) + L"' has a malformed binary header; not promoted.");
        return 1;
    }
    VerboseLine(L"Register '" + ToWide(name) + L"' is now the clipboard everywhere.");
    return 0;
}

int RunRegisterLs(const std::string& patternArg, bool verbose) {
    const std::string pattern = clipp_platform_detail::NormalizeUtf8Canonical(patternArg);
    if (!g_keyManager.HaveNetworkKey()) {
        ErrLine(L"No group key configured. Run `clipp key set` first.");
        return 1;
    }
    NetworkStartup net;
    if (!net.ok) {
        ErrLine(L"Failed to initialize networking.");
        return 1;
    }
    HostId localHostId;
    g_settings.ensureHostID(localHostId);
    const std::string localHostName =
        clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);

    std::vector<RegisterWire::RegisterListEntry> entries;
    bool got = false;
    bool filterMatched = false;
    const bool reached = WithRegisterGateway(localHostId, localHostName, filterMatched, [&](OneShotPeer& conn) -> bool {
        if (!conn.SendFrame("RLST")) {
            return false;
        }
        std::vector<unsigned char> frame;
        while (conn.RecvFrame(frame)) {
            if (frame.size() < 4) {
                break;
            }
            if (std::memcmp(frame.data(), "RLST", 4) == 0) {
                const std::vector<unsigned char> fbody(frame.begin() + 4, frame.end());
                if (!RegisterWire::TryDecodeList(fbody, entries)) {
                    return false;
                }
                got = true;
                return true;
            }
            if (std::memcmp(frame.data(), "SYNC", 4) == 0) {
                conn.SendFrame("EOSY");
                continue;
            }
            // ignore crosstalk
        }
        return false;
    });
    if (!reached) {
        ReportNoGateway(L"Could not reach any register-capable device.", filterMatched);
        return 1;
    }
    if (!got) {
        return 0;
    }
    // Entries arrive name-sorted from the gateway. An empty pattern lists all; the ""
    // default-mirror entry matches only an empty/`*` pattern.
    std::vector<const RegisterWire::RegisterListEntry*> shown;
    for (const auto& e : entries) {
        if (pattern.empty() || GlobMatch(pattern, e.name)) {
            shown.push_back(&e);
        }
    }
    if (verbose) {
        FormatListVerbose(shown);
    } else {
        for (const auto* e : shown) {
            OutLine(e->name.empty() ? L"(clipboard)" : ToWide(e->name));
        }
    }
    return 0;
}

int RunRegisterRm(const std::string& patternArg) {
    const std::string pattern = clipp_platform_detail::NormalizeUtf8Canonical(patternArg);
    const bool wildcard = HasWildcard(pattern);
    if (!wildcard && !IsValidRegisterName(pattern)) {
        ErrLine(L"Invalid register name: up to 64 bytes of printable UTF-8; '?' '*' '/' are reserved; no leading/trailing space.");
        return 1;
    }
    if (!g_keyManager.HaveNetworkKey()) {
        ErrLine(L"No group key configured. Run `clipp key set` first.");
        return 1;
    }
    NetworkStartup net;
    if (!net.ok) {
        ErrLine(L"Failed to initialize networking.");
        return 1;
    }
    HostId localHostId;
    g_settings.ensureHostID(localHostId);
    const std::string localHostName =
        clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);

    int removed = 0;
    bool absent = false;    // exact name not present
    bool noMatch = false;   // glob matched nothing
    bool filterMatched = false;
    const bool reached = WithRegisterGateway(localHostId, localHostName, filterMatched, [&](OneShotPeer& conn) -> bool {
        std::vector<std::string> targets;
        if (wildcard) {
            // List, match the glob (never the "" default mirror), then tombstone each.
            if (!conn.SendFrame("RLST")) {
                return false;
            }
            std::vector<RegisterWire::RegisterListEntry> entries;
            bool gotList = false;
            std::vector<unsigned char> frame;
            while (conn.RecvFrame(frame)) {
                if (frame.size() < 4) {
                    break;
                }
                if (std::memcmp(frame.data(), "RLST", 4) == 0) {
                    const std::vector<unsigned char> fbody(frame.begin() + 4, frame.end());
                    if (!RegisterWire::TryDecodeList(fbody, entries)) {
                        return false;
                    }
                    gotList = true;
                    break;
                }
                if (std::memcmp(frame.data(), "SYNC", 4) == 0) {
                    conn.SendFrame("EOSY");
                    continue;
                }
            }
            if (!gotList) {
                return false;
            }
            for (const auto& e : entries) {
                if (!e.name.empty() && GlobMatch(pattern, e.name)) {
                    targets.push_back(e.name);
                }
            }
            if (targets.empty()) {
                noMatch = true;
                return true;
            }
        } else {
            targets.push_back(pattern);
        }

        for (const std::string& target : targets) {
            RegisterRecord rec;
            rec.name = target;
            rec.originHostId = localHostId;
            rec.flags = RegisterFlags::Tombstone;   // empty value; the gateway re-stamps the HLC
            const auto regw = RegisterWire::EncodeRecord(rec, RegisterWire::Transport::Relay);
            if (!conn.SendFrame("REGW", regw.data(), static_cast<uint32_t>(regw.size()))) {
                return false;
            }
            std::vector<unsigned char> frame;
            bool acked = false;
            while (conn.RecvFrame(frame)) {
                if (frame.size() < 4) {
                    break;
                }
                if (std::memcmp(frame.data(), "RDEL", 4) == 0) { ++removed; acked = true; break; }
                if (std::memcmp(frame.data(), "NONE", 4) == 0) { if (!wildcard) absent = true; acked = true; break; }
                if (std::memcmp(frame.data(), "SYNC", 4) == 0) { conn.SendFrame("EOSY"); continue; }
                // ignore crosstalk
            }
            if (!acked) {
                return false;   // connection died mid-batch
            }
        }
        return true;
    });
    if (!reached) {
        ReportNoGateway(L"Could not reach any register-capable device.", filterMatched);
        return 1;
    }
    if (noMatch) {
        ErrLine(L"No registers match '" + ToWide(pattern) + L"'.");
        return 1;
    }
    if (absent) {
        ErrLine(L"No such register '" + ToWide(pattern) + L"'.");
        return 1;
    }
    VerboseLine(L"Removed " + std::to_wstring(removed) + L" register(s).");
    return 0;
}

// Refreshes the expiry timer on matching registers — `clipp touch <name-or-glob>`.
// Entirely client-side, no protocol addition and no capability gate: RGET already
// refreshes `touched` at the serving gateway as a side effect (the documented
// read-refreshes semantic), so a touch is just a read whose value we discard. The
// bump then travels on normal anti-entropy — the RSYN digest carries `touched`, and
// peers holding a staler one get the record pushed — so it works against every
// deployed gateway. (Transport::TouchOnly stays reserved for a value-free gateway
// op if register sizes ever make shipping the bytes matter.)
int RunTouch(const std::string& patternArg) {
    const std::string pattern = clipp_platform_detail::NormalizeUtf8Canonical(patternArg);
    const bool wildcard = HasWildcard(pattern);
    if (!wildcard && !IsValidRegisterName(pattern)) {
        ErrLine(L"Invalid register name: up to 64 bytes of printable UTF-8; '?' '*' '/' are reserved; no leading/trailing space.");
        return 1;
    }
    if (!g_keyManager.HaveNetworkKey()) {
        ErrLine(L"No group key configured. Run `clipp key set` first.");
        return 1;
    }
    NetworkStartup net;
    if (!net.ok) {
        ErrLine(L"Failed to initialize networking.");
        return 1;
    }
    HostId localHostId;
    g_settings.ensureHostID(localHostId);
    const std::string localHostName =
        clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);

    int touched = 0;
    bool absent = false;    // exact name not present
    bool noMatch = false;   // glob matched nothing
    bool filterMatched = false;
    const bool reached = WithRegisterGateway(localHostId, localHostName, filterMatched, [&](OneShotPeer& conn) -> bool {
        std::vector<std::string> targets;
        if (wildcard) {
            // List, match the glob (never the "" default mirror), then read each.
            if (!conn.SendFrame("RLST")) {
                return false;
            }
            std::vector<RegisterWire::RegisterListEntry> entries;
            bool gotList = false;
            std::vector<unsigned char> frame;
            while (conn.RecvFrame(frame)) {
                if (frame.size() < 4) {
                    break;
                }
                if (std::memcmp(frame.data(), "RLST", 4) == 0) {
                    const std::vector<unsigned char> fbody(frame.begin() + 4, frame.end());
                    if (!RegisterWire::TryDecodeList(fbody, entries)) {
                        return false;
                    }
                    gotList = true;
                    break;
                }
                if (std::memcmp(frame.data(), "SYNC", 4) == 0) {
                    conn.SendFrame("EOSY");
                    continue;
                }
            }
            if (!gotList) {
                return false;
            }
            for (const auto& e : entries) {
                if (!e.name.empty() && GlobMatch(pattern, e.name)) {
                    targets.push_back(e.name);
                }
            }
            if (targets.empty()) {
                noMatch = true;
                return true;
            }
        } else {
            targets.push_back(pattern);
        }

        for (const std::string& target : targets) {
            const std::vector<unsigned char> req = RegisterWire::EncodeName(target);
            if (!conn.SendFrame("RGET", req.data(), static_cast<uint32_t>(req.size()))) {
                return false;
            }
            std::vector<unsigned char> frame;
            bool answered = false;
            while (conn.RecvFrame(frame)) {
                if (frame.size() < 4) {
                    break;
                }
                if (std::memcmp(frame.data(), "REGW", 4) == 0) {
                    RegisterRecord rec;
                    uint8_t transport = 0;
                    const std::vector<unsigned char> fbody(frame.begin() + 4, frame.end());
                    if (!RegisterWire::TryDecodeRecord(fbody, rec, transport)) {
                        return false;
                    }
                    if (rec.name != target) {
                        continue;  // an unsolicited broadcast for another register; keep waiting
                    }
                    ++touched;  // value discarded; the read itself was the point
                    answered = true;
                    break;
                }
                if (std::memcmp(frame.data(), "NONE", 4) == 0) {
                    // Exact name: report absent. Mid-glob: the register expired
                    // between the list and the read; skip it.
                    if (!wildcard) absent = true;
                    answered = true;
                    break;
                }
                if (std::memcmp(frame.data(), "SYNC", 4) == 0) { conn.SendFrame("EOSY"); continue; }
                // ignore crosstalk
            }
            if (!answered) {
                return false;   // connection died mid-batch
            }
        }
        return true;
    });
    if (!reached) {
        ReportNoGateway(L"Could not reach any register-capable device.", filterMatched);
        return 1;
    }
    if (noMatch) {
        ErrLine(L"No registers match '" + ToWide(pattern) + L"'.");
        return 1;
    }
    if (absent) {
        ErrLine(L"No such register '" + ToWide(pattern) + L"'.");
        return 1;
    }
    VerboseLine(L"Touched " + std::to_wstring(touched) + L" register(s).");
    return 0;
}

// ---- Discovery diagnostics: clipp peers / probe -----------------------------

std::wstring OsLabel(OsType os) {
    switch (os) {
    case OsType::Windows:    return L"Windows";
    case OsType::MacOS:      return L"macOS";
    case OsType::IOS_iPhone: return L"iPhone";
    case OsType::IOS_iPad:   return L"iPad";
    case OsType::Linux:      return L"Linux";
    case OsType::Unknown:    break;
    }
    return L"?";
}

// One streaming browse, deduped by (hostId, ip, port). includeSelf=true so this
// device's own running GUI shows up. Runs to the browse ceiling (the callback never
// stops it) so the full neighbor set is collected. Needs the network key set, or the
// TXT records don't decrypt and nothing surfaces (the caller checks that first).
std::vector<MDNSDiscovery::DiscoveredPeer> CollectPeers() {
    std::vector<MDNSDiscovery::DiscoveredPeer> peers;
    std::vector<std::wstring> seen;
    MDNSDiscovery::BrowseStream(OneShot::kBrowseCeiling, /*includeSelf=*/true,
        [&](const MDNSDiscovery::DiscoveredPeer& peer) -> bool {
            const std::wstring key =
                peer.hostId.ToHexWString() + L"/" + ToWide(peer.ip) + L"/" + std::to_wstring(peer.port);
            if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
                seen.push_back(key);
                peers.push_back(peer);
            }
            return true;  // keep browsing until the ceiling; collect everyone
        });
    return peers;
}

// Aligned table to stdout: a header row plus data rows, columns left-aligned, two
// spaces between, no trailing pad on the last column. Shared by peers/probe.
void PrintTable(const std::vector<std::wstring>& headers,
                const std::vector<std::vector<std::wstring>>& rows) {
    std::vector<size_t> width(headers.size(), 0);
    for (size_t c = 0; c < headers.size(); ++c) width[c] = headers[c].size();
    for (const auto& row : rows)
        for (size_t c = 0; c < row.size() && c < width.size(); ++c)
            width[c] = (std::max)(width[c], row[c].size());
    const auto emit = [&](const std::vector<std::wstring>& row) {
        std::wstring line;
        for (size_t c = 0; c < width.size(); ++c) {
            const std::wstring cell = c < row.size() ? row[c] : std::wstring();
            line += (c + 1 == width.size()) ? cell : PadRight(cell, width[c]) + L"  ";
        }
        OutLine(line);
    };
    emit(headers);
    for (const auto& row : rows) emit(row);
}

// `peers`: pure discovery. Lists every device the network browse surfaces (name, IP,
// port, OS, host id) with no connection attempt — so it answers "what does clipp see?"
// even when connections are failing, and shows the exact name/IP strings `--host` wants.
int RunPeers() {
    if (!g_keyManager.HaveNetworkKey()) {
        ErrLine(L"No group key configured. Run `clipp key set` first.");
        return 1;
    }
    NetworkStartup net;
    if (!net.ok) {
        ErrLine(L"Failed to initialize networking.");
        return 1;
    }
    HostId localHostId;
    g_settings.ensureHostID(localHostId);

    VerboseLine(L"Browsing for peers...");
    const std::vector<MDNSDiscovery::DiscoveredPeer> peers = CollectPeers();
    if (peers.empty()) {
        ErrLine(L"No devices discovered on the network.");
        return 1;
    }

    std::vector<std::vector<std::wstring>> rows;
    for (const auto& peer : peers) {
        const std::wstring name = peer.deviceName.empty() ? L"(unknown)" : SanitizePreview(peer.deviceName);
        const std::wstring tag = (peer.hostId == localHostId) ? L"[this device]" : L"";
        // 17 chars = the first two dash-separated 8-hex groups (a clean boundary; the
        // full id is 4 groups). Enough to disambiguate devices on a normal network.
        rows.push_back({name, ToWide(peer.ip), std::to_wstring(peer.port),
                        OsLabel(peer.osType), peer.hostId.ToHexWString().substr(0, 17), tag});
    }
    PrintTable({L"NAME", L"IP", L"PORT", L"OS", L"HOST ID", L""}, rows);
    return 0;
}

// `probe`: the active counterpart to `peers`. Connects to each discovered device and
// reports reachability and advertised capabilities (paste = serves recent clipboard,
// registers = serves the named-register protocol). Slower than `peers` — it handshakes
// with every device, including this one — but it's how you confirm WHY a verb picked,
// or skipped, a given gateway.
int RunProbe() {
    if (!g_keyManager.HaveNetworkKey()) {
        ErrLine(L"No group key configured. Run `clipp key set` first.");
        return 1;
    }
    NetworkStartup net;
    if (!net.ok) {
        ErrLine(L"Failed to initialize networking.");
        return 1;
    }
    HostId localHostId;
    g_settings.ensureHostID(localHostId);
    const std::string localHostName =
        clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);

    VerboseLine(L"Browsing for peers...");
    const std::vector<MDNSDiscovery::DiscoveredPeer> peers = CollectPeers();
    if (peers.empty()) {
        ErrLine(L"No devices discovered on the network.");
        return 1;
    }

    std::vector<std::vector<std::wstring>> rows;
    for (const auto& peer : peers) {
        VerboseLine(L"Probing " + PeerLabel(peer, localHostId) + L"...");
        OneShotPeer connection;
        const bool reachable = connection.Connect(peer.ip, peer.port, localHostId, localHostName,
                                                  peer.hostId, OneShot::kConnectTimeout,
                                                  OneShot::kSessionTimeout);
        std::wstring paste = L"-", registers = L"-", put = L"-";
        if (reachable) {
            const auto& caps = connection.RemoteCaps();
            paste     = (caps[0] & CryptoChannel::CAP0_SERVES_RECENT)    ? L"yes" : L"no";
            registers = (caps[0] & CryptoChannel::CAP0_SERVES_REGISTERS) ? L"yes" : L"no";
            put       = (caps[0] & CryptoChannel::CAP0_SERVES_PUT)       ? L"yes" : L"no";
        }
        const std::wstring name = peer.deviceName.empty() ? L"(unknown)" : SanitizePreview(peer.deviceName);
        const std::wstring tag = (peer.hostId == localHostId) ? L"[this device]" : L"";
        rows.push_back({name, ToWide(peer.ip), reachable ? L"yes" : L"no", paste, registers, put, tag});
    }
    PrintTable({L"NAME", L"IP", L"REACHABLE", L"PASTE", L"REGISTERS", L"PUT", L""}, rows);
    return 0;
}

// ---- Mesh map: clipp map ------------------------------------------------------

// One parsed `conn` record from a peer's NMAP report: that peer's view of one
// remote host (counts aggregate its live connections to that host).
struct MapConnection {
    uint64_t in = 0;
    uint64_t out = 0;
    std::wstring state;      // outgoing-direction state; meaningful when out > 0
    uint64_t ageSeconds = 0;
    uint64_t tx = 0;
    uint64_t rx = 0;
    std::wstring os;
    std::wstring idPrefix;   // first hex group, for display
    std::wstring name;       // control-char scrubbed for terminal output
};

// Splits a report line's "k=v k=v ... name=<rest>" tail into pairs. `name` is by
// convention the LAST key and consumes the remainder of the line (device names
// contain spaces); unknown keys pass through for the caller to ignore.
std::vector<std::pair<std::string, std::string>> ParseReportFields(const std::string& tail) {
    std::vector<std::pair<std::string, std::string>> fields;
    size_t pos = 0;
    while (pos < tail.size()) {
        while (pos < tail.size() && tail[pos] == ' ') ++pos;
        if (pos >= tail.size()) break;
        if (tail.compare(pos, 5, "name=") == 0) {
            fields.emplace_back("name", tail.substr(pos + 5));
            break;
        }
        const size_t eq = tail.find('=', pos);
        if (eq == std::string::npos) break;  // malformed tail; keep what parsed so far
        const size_t end = tail.find(' ', eq + 1);
        fields.emplace_back(tail.substr(pos, eq - pos),
                            end == std::string::npos ? tail.substr(eq + 1)
                                                     : tail.substr(eq + 1, end - eq - 1));
        pos = (end == std::string::npos) ? tail.size() : end + 1;
    }
    return fields;
}

uint64_t ParseU64(const std::string& s) {
    return std::strtoull(s.c_str(), nullptr, 10);
}

// Asks every discovered device for its connection table (NMAP, gated on
// CAP0_SERVES_NETMAP) and renders a mesh-wide health view: one line per device
// with its total incoming/outgoing connection counts; -v adds that device's
// per-peer rows, -vv the raw report text (which also shows keys newer daemons
// emit that this build doesn't render). The GUI already shows THIS device's
// health; the point of `map` is every other node's view without walking to it.
int RunMap(int verbosity) {
    if (!g_keyManager.HaveNetworkKey()) {
        ErrLine(L"No group key configured. Run `clipp key set` first.");
        return 1;
    }
    NetworkStartup net;
    if (!net.ok) {
        ErrLine(L"Failed to initialize networking.");
        return 1;
    }
    HostId localHostId;
    g_settings.ensureHostID(localHostId);
    const std::string localHostName =
        clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);

    VerboseLine(L"Browsing for peers...");
    const std::vector<MDNSDiscovery::DiscoveredPeer> peers = CollectPeers();
    if (peers.empty()) {
        ErrLine(L"No devices discovered on the network.");
        return 1;
    }

    struct HostReport {
        enum class Status { Unreachable, NoSupport, BadReport, Ok };
        HostId hostId;
        std::wstring name;                   // discovery name until the self-report improves it
        std::wstring version;
        bool thisDevice = false;
        Status status = Status::Unreachable;
        uint64_t totalIn = 0;
        uint64_t totalOut = 0;
        std::vector<MapConnection> conns;
        std::vector<std::wstring> rawLines;  // -vv
    };
    std::vector<HostReport> reports;  // discovery order
    auto findReport = [&](const HostId& id) -> HostReport& {
        for (auto& r : reports) {
            if (r.hostId == id) return r;
        }
        reports.push_back({});
        reports.back().hostId = id;
        return reports.back();
    };

    for (const auto& peer : peers) {
        HostReport& report = findReport(peer.hostId);
        if (report.name.empty()) {
            report.name = peer.deviceName.empty() ? L"(unknown)" : SanitizePreview(peer.deviceName);
            report.thisDevice = (peer.hostId == localHostId);
        }
        if (report.status != HostReport::Status::Unreachable) {
            continue;  // this host already answered via another address
        }
        VerboseLine(L"Querying " + PeerLabel(peer, localHostId) + L"...");
        OneShotPeer connection;
        if (!connection.Connect(peer.ip, peer.port, localHostId, localHostName, peer.hostId,
                                OneShot::kConnectTimeout, OneShot::kSessionTimeout)) {
            VerboseLine(L"  unreachable.");
            continue;  // a later address for the same host may still answer
        }
        if ((connection.RemoteCaps()[0] & CryptoChannel::CAP0_SERVES_NETMAP) == 0) {
            report.status = HostReport::Status::NoSupport;
            continue;
        }
        if (!connection.SendFrame("NMAP")) {
            continue;
        }
        std::string body;
        bool answered = false;
        std::vector<unsigned char> frame;
        while (connection.RecvFrame(frame)) {
            if (frame.size() < 4) {
                break;
            }
            if (std::memcmp(frame.data(), "NMAP", 4) == 0) {
                body.assign(frame.begin() + 4, frame.end());
                answered = true;
                break;
            }
            if (std::memcmp(frame.data(), "NONE", 4) == 0) {
                report.status = HostReport::Status::NoSupport;
                answered = true;
                break;
            }
            if (std::memcmp(frame.data(), "SYNC", 4) == 0) { connection.SendFrame("EOSY"); continue; }
            // Ignore RSYN/REGW/CLIP and other crosstalk; keep waiting for the reply.
        }
        if (!answered || report.status == HostReport::Status::NoSupport) {
            continue;
        }

        // Parse: a "netmap" header line, then one record per line ("self ...",
        // "conn ..."). Unknown record types and keys are ignored (extensibility).
        report.status = HostReport::Status::BadReport;
        bool sawHeader = false;
        size_t lineStart = 0;
        while (lineStart < body.size()) {
            const size_t lineEnd = body.find('\n', lineStart);
            const std::string line = body.substr(
                lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
            lineStart = (lineEnd == std::string::npos) ? body.size() : lineEnd + 1;
            if (line.empty()) continue;
            if (verbosity >= 2) report.rawLines.push_back(SanitizePreview(line));
            const size_t space = line.find(' ');
            const std::string tag = line.substr(0, space);
            const std::string tail = (space == std::string::npos) ? std::string() : line.substr(space + 1);
            if (tag == "netmap") { sawHeader = true; continue; }
            if (!sawHeader) break;  // not our format; bail (status stays BadReport)
            if (tag == "self") {
                for (const auto& [key, value] : ParseReportFields(tail)) {
                    if (key == "ver") report.version = ToWide(value);
                    else if (key == "name" && !value.empty()) report.name = SanitizePreview(value);
                }
                continue;
            }
            if (tag == "conn") {
                MapConnection conn;
                for (const auto& [key, value] : ParseReportFields(tail)) {
                    if      (key == "in")    conn.in = ParseU64(value);
                    else if (key == "out")   conn.out = ParseU64(value);
                    else if (key == "state") conn.state = ToWide(value);
                    else if (key == "age")   conn.ageSeconds = ParseU64(value);
                    else if (key == "tx")    conn.tx = ParseU64(value);
                    else if (key == "rx")    conn.rx = ParseU64(value);
                    else if (key == "os")    conn.os = ToWide(value);
                    else if (key == "id")    conn.idPrefix = ToWide(value).substr(0, 8);
                    else if (key == "name")  conn.name = SanitizePreview(value);
                }
                report.totalIn += conn.in;
                report.totalOut += conn.out;
                report.conns.push_back(std::move(conn));
                continue;
            }
            // Unknown record type from a newer daemon: skip the line.
        }
        if (sawHeader) {
            report.status = HostReport::Status::Ok;
        }
    }

    // Render. Default: one line per device with its in/out totals. In a healthy
    // N-daemon mesh every daemon shows (N-1)/(N-1).
    size_t nameWidth = 0;
    for (const auto& r : reports) nameWidth = (std::max)(nameWidth, r.name.size());
    for (const auto& r : reports) {
        std::wstring line = PadRight(r.name, nameWidth) + L"  ";
        switch (r.status) {
        case HostReport::Status::Ok:
            line += std::to_wstring(r.totalIn) + L"/" + std::to_wstring(r.totalOut);
            break;
        case HostReport::Status::Unreachable: line += L"unreachable";        break;
        case HostReport::Status::NoSupport:   line += L"no map support";     break;
        case HostReport::Status::BadReport:   line += L"unrecognized reply"; break;
        }
        if (verbosity >= 1 && !r.version.empty()) line += L"  v" + r.version;
        if (r.thisDevice) line += L"  [this device]";
        OutLine(line);
        if (verbosity >= 1 && r.status == HostReport::Status::Ok) {
            size_t connNameWidth = 0;
            for (const auto& c : r.conns) connNameWidth = (std::max)(connNameWidth, c.name.size());
            for (const auto& c : r.conns) {
                OutLine(L"  " + PadRight(c.name, connNameWidth) +
                        L"  in " + std::to_wstring(c.in) + L"  out " + std::to_wstring(c.out) +
                        L"  " + PadRight(c.out > 0 ? c.state : std::wstring(L"-"), 10) +
                        L"  " + PadLeft(FormatAgeSeconds(c.ageSeconds), 4) +
                        L"  tx " + PadLeft(HumanizeSize(c.tx), 5) +
                        L"  rx " + PadLeft(HumanizeSize(c.rx), 5) +
                        L"  " + c.os + L" " + c.idPrefix);
            }
        }
        if (verbosity >= 2) {
            for (const auto& raw : r.rawLines) {
                OutLine(L"  | " + raw);
            }
        }
    }
    return 0;
}

// `version`: the full 4-part W.X.Y.Z build stamp on stdout. --help's header and
// the GUI About screens show the 3-part marketing form; this is the diagnostic
// that includes the build counter (0.0.0.0 = unstamped dev build — the release
// version is tag-canonical, injected at build time; see RELEASING.md).
int RunVersion() {
    OutLine(L"clipp " + ToWide(CLIPP_VERSION_STRING));
    return 0;
}

enum class Action { None, Gui, KeySet, KeyErase, KeyShow, HostIdShow, HostIdReset, Copy, Paste, Put, RegLs, RegRm, RegTouch, Peers, Probe, Map, Version };

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
    app.footer("With no command, redirection picks the verb: `x | clipp` copies, `clipp > f` pastes, "
               "and both at once copies while passing stdin through. Scripts should spell out copy/paste.");

    std::string logLevel;
    CLI::Option* logLevelOption = app.add_option(
        "--loglevel", logLevel,
        "Log level: error, warn, info, debug, ddebug (default: silent in command mode)");

    int verbose = 0;  // counted: -v / -vv (some commands render extra detail per level)
    app.add_flag("-v,--verbose", verbose, "Print progress to stderr; repeat (-vv) for more detail (map)");

    std::string hostFilter;
    app.add_option("--host", hostFilter,
        "Talk only to the device with this name (case-insensitive) or IP, instead of the first peer found (copy/paste/ls/rm)");

    // Same text as the `version` subcommand. Routed through CLI11's version
    // machinery, so like --help it prints and exits 0 via app.exit() below.
    app.set_version_flag("--version", "clipp " CLIPP_VERSION_STRING);

    Action action = Action::None;

    std::string copyRegisterName;
    bool copyPrivate = false;
    CLI::App* copyCommand = app.add_subcommand("copy", "Read stdin and copy it to the network, or to a named register");
    copyCommand->alias("c");
    copyCommand->add_option("name", copyRegisterName, "Named register (default: the shared clipboard)");
    copyCommand->add_flag("--private", copyPrivate, "Mask the value in `ls -v` and refuse to print it to a terminal on paste");
    copyCommand->callback([&]() { action = Action::Copy; });

    std::string pasteRegisterName;
    CLI::App* pasteCommand = app.add_subcommand("paste", "Write the newest clipboard item, or a named register, to stdout");
    pasteCommand->alias("p");
    pasteCommand->add_option("name", pasteRegisterName, "Named register (default: the shared clipboard)");
    pasteCommand->callback([&]() { action = Action::Paste; });

    std::string putRegisterName;
    CLI::App* putCommand = app.add_subcommand("put", "Make a named register the current clipboard on all devices");
    putCommand->add_option("name", putRegisterName, "Register to promote")->required();
    putCommand->callback([&]() { action = Action::Put; });

    std::string lsPattern;
    CLI::App* lsCommand = app.add_subcommand("ls", "List named registers (-v for details; optional name/glob filter)");
    lsCommand->add_option("pattern", lsPattern, "Only list registers matching this name or glob (? and *)");
    lsCommand->callback([&]() { action = Action::RegLs; });

    std::string rmRegisterName;
    CLI::App* rmCommand = app.add_subcommand("rm", "Delete a named register (name or glob)");
    rmCommand->add_option("name", rmRegisterName, "Register name or glob (? and *) to delete")->required();
    rmCommand->callback([&]() { action = Action::RegRm; });

    std::string touchPattern;
    CLI::App* touchCommand = app.add_subcommand("touch", "Refresh the expiry timer on registers (name or glob)");
    touchCommand->add_option("name", touchPattern, "Register name or glob (? and *) to refresh")->required();
    touchCommand->callback([&]() { action = Action::RegTouch; });

    CLI::App* peersCommand = app.add_subcommand(
        "peers", "List devices discovered on the network (name, IP, port, OS) — these are the names/IPs for --host");
    peersCommand->callback([&]() { action = Action::Peers; });

    CLI::App* probeCommand = app.add_subcommand(
        "probe", "Connect to each discovered device and report reachability + capabilities");
    probeCommand->callback([&]() { action = Action::Probe; });

    CLI::App* mapCommand = app.add_subcommand(
        "map", "Mesh health: every device's connection table (-v per-peer rows, -vv raw reports)");
    mapCommand->callback([&]() { action = Action::Map; });

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

    CLI::App* versionCommand = app.add_subcommand("version", "Print the clipp version (full 4-part build stamp)");
    versionCommand->callback([&]() { action = Action::Version; });

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

    // Bare launch with redirection: infer the verb from the stdio dispositions.
    // `x | clipp` copies, `clipp > f` pastes, and both redirected at once copies
    // then tees stdin through to stdout (the paste half never re-reads the network —
    // the bytes are in hand). Only a real pipe or regular file counts: terminals are
    // interactive (usage below), and Absent streams (NUL, /dev/null, no handle:
    // Task Scheduler, cron, launchd, Dock) must never trigger a verb, or a scheduled
    // bare launch would block on stdin or clobber the group clipboard. Explicit
    // copy/paste remain the canonical spellings for scripts.
    bool inferredEchoToStdout = false;
    if (action == Action::None) {
        const StreamDisposition inDisp = StdinDisposition();
        const StreamDisposition outDisp = StdoutDisposition();
        const auto redirected = [](StreamDisposition d) {
            return d == StreamDisposition::Pipe || d == StreamDisposition::File;
        };
#ifndef CLIPP_HEADLESS
        // GUI-style launch guard. Dock / double-click / autostart / wrapper-launcher
        // contexts show no stdin, at most a pipe-or-null stdout (macOS LaunchServices
        // points stdout at /dev/null or a logging pipe — never a regular file), and
        // no terminal on stderr. Those must fall through to the GUI untouched, even
        // though stdout can look like a redirect.
        const bool guiStyleLaunch = inDisp == StreamDisposition::Absent &&
                                    (outDisp == StreamDisposition::Absent ||
                                     outDisp == StreamDisposition::Pipe) &&
                                    StderrDisposition() != StreamDisposition::Terminal;
#else
        const bool guiStyleLaunch = false;  // no GUI to fall through to
#endif
        if (!guiStyleLaunch) {
            if (redirected(inDisp)) {
                action = Action::Copy;
                inferredEchoToStdout = redirected(outDisp);
            } else if (redirected(outDisp)) {
                action = Action::Paste;
            }
        }
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

    g_verbose = verbose > 0;
    g_hostFilter = hostFilter;

    // Bare launch with no command and no inferred verb: print usage and exit instead
    // of silently launching the terminal-blocking GUI. On desktop this is gated on a console (a
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
        return copyRegisterName.empty() ? RunCopy(inferredEchoToStdout)
                                        : RunRegisterCopy(copyRegisterName, copyPrivate);
    case Action::Paste:
        return pasteRegisterName.empty() ? RunPaste() : RunRegisterPaste(pasteRegisterName);
    case Action::Put:
        return RunPut(putRegisterName);
    case Action::RegLs:
        return RunRegisterLs(lsPattern, verbose > 0);
    case Action::RegRm:
        return RunRegisterRm(rmRegisterName);
    case Action::RegTouch:
        return RunTouch(touchPattern);
    case Action::Peers:
        return RunPeers();
    case Action::Probe:
        return RunProbe();
    case Action::Map:
        return RunMap(verbose);
    case Action::Version:
        return RunVersion();
    case Action::Gui:
    case Action::None:
        break;
    }
    return std::nullopt;
}

}  // namespace clipp::cli
