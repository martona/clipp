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

// ---- Named registers: clipp copy/paste/ls/rm <name> -------------------------

bool StdoutIsTty() {
#ifdef _WIN32
    return StdHandleIsConsole(STD_OUTPUT_HANDLE);
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

std::wstring FormatAge(const Hlc& touched) {
    const uint64_t now = HlcClock::SystemNowMs();
    const uint64_t ageMs = (now > touched.wallMs) ? (now - touched.wallMs) : 0;
    const uint64_t s = ageMs / 1000;
    if (s < 60)    return std::to_wstring(s) + L"s";
    if (s < 3600)  return std::to_wstring(s / 60) + L"m";
    if (s < 86400) return std::to_wstring(s / 3600) + L"h";
    return std::to_wstring(s / 86400) + L"d";
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
std::wstring OriginLabel(const HostId& host) {
    const std::wstring hex = host.ToHexWString(HostId::kSize);
    return hex.substr(0, 8);
}

std::wstring SanitizePreview(const std::string& preview) {
    std::string s = preview;
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = '.';   // control chars (incl newlines/tabs) -> dot
    }
    return ToWide(s);
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

// `ls -v`: an aligned name / age / size / origin / contents table, contents
// sanitized + width-capped with an overflow marker on a tty; private masked.
void FormatListVerbose(const std::vector<const RegisterWire::RegisterListEntry*>& entries) {
    size_t wName = 0, wAge = 0, wSize = 0, wOrigin = 0;
    for (const auto* e : entries) {
        const std::wstring dn = e->name.empty() ? L"(clipboard)" : ToWide(e->name);
        wName = (std::max)(wName, dn.size());
        wAge = (std::max)(wAge, FormatAge(e->touched).size());
        wSize = (std::max)(wSize, HumanizeSize(e->valueSize).size());
        wOrigin = (std::max)(wOrigin, OriginLabel(e->originHostId).size());
    }
    const int termW = TerminalWidth();
    const bool tty = StdoutIsTty();
    for (const auto* e : entries) {
        const std::wstring dn = e->name.empty() ? L"(clipboard)" : ToWide(e->name);
        std::wstring line = PadRight(dn, wName) + L"  " + PadLeft(FormatAge(e->touched), wAge) + L"  " +
                            PadLeft(HumanizeSize(e->valueSize), wSize) + L"  " +
                            PadRight(OriginLabel(e->originHostId), wOrigin) + L"  ";
        std::wstring contents =
            (e->flags & RegisterFlags::Private) ? L"[private]" : SanitizePreview(e->preview);
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
template <typename Fn>
bool WithRegisterGateway(const HostId& localHostId, const std::string& localHostName, Fn exchange) {
    return MDNSDiscovery::BrowseStream(OneShot::kBrowseCeiling, /*includeSelf=*/true,
        [&](const MDNSDiscovery::DiscoveredPeer& peer) -> bool {
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

int RunRegisterCopy(const std::string& name, bool isPrivate) {
    if (!IsValidRegisterName(name)) {
        ErrLine(L"Invalid register name. Use 1-64 characters of [a-z0-9._-].");
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
    const bool reached = WithRegisterGateway(localHostId, localHostName, [&](OneShotPeer& conn) -> bool {
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
        ErrLine(L"Could not reach any register-capable device.");
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

int RunRegisterPaste(const std::string& name) {
    if (!IsValidRegisterName(name)) {
        ErrLine(L"Invalid register name. Use 1-64 characters of [a-z0-9._-].");
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
    bool refusedPrivate = false;
    const bool reached = WithRegisterGateway(localHostId, localHostName, [&](OneShotPeer& conn) -> bool {
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
                if (rec.IsPrivate() && StdoutIsTty()) {
                    ErrLine(L"Register '" + ToWide(name) +
                            L"' is private; refusing to print to a terminal. Pipe it to read.");
                    refusedPrivate = true;
                } else {
                    WriteAllStdout(reinterpret_cast<const unsigned char*>(rec.value.data()),
                                   rec.value.size());
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
        ErrLine(L"Could not reach any register-capable device.");
        return 1;
    }
    if (!present) {
        ErrLine(L"No such register '" + ToWide(name) + L"'.");
        return 1;
    }
    return refusedPrivate ? 1 : 0;
}

int RunRegisterLs(const std::string& pattern, bool verbose) {
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
    const bool reached = WithRegisterGateway(localHostId, localHostName, [&](OneShotPeer& conn) -> bool {
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
        ErrLine(L"Could not reach any register-capable device.");
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

int RunRegisterRm(const std::string& pattern) {
    const bool wildcard = HasWildcard(pattern);
    if (!wildcard && !IsValidRegisterName(pattern)) {
        ErrLine(L"Invalid register name. Use 1-64 characters of [a-z0-9._-].");
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
    const bool reached = WithRegisterGateway(localHostId, localHostName, [&](OneShotPeer& conn) -> bool {
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
        ErrLine(L"Could not reach any register-capable device.");
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

enum class Action { None, Gui, KeySet, KeyErase, KeyShow, HostIdShow, HostIdReset, Copy, Paste, RegLs, RegRm };

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

    std::string lsPattern;
    CLI::App* lsCommand = app.add_subcommand("ls", "List named registers (-v for details; optional name/glob filter)");
    lsCommand->add_option("pattern", lsPattern, "Only list registers matching this name or glob (? and *)");
    lsCommand->callback([&]() { action = Action::RegLs; });

    std::string rmRegisterName;
    CLI::App* rmCommand = app.add_subcommand("rm", "Delete a named register (name or glob)");
    rmCommand->add_option("name", rmRegisterName, "Register name or glob (? and *) to delete")->required();
    rmCommand->callback([&]() { action = Action::RegRm; });

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
        return copyRegisterName.empty() ? RunCopy() : RunRegisterCopy(copyRegisterName, copyPrivate);
    case Action::Paste:
        return pasteRegisterName.empty() ? RunPaste() : RunRegisterPaste(pasteRegisterName);
    case Action::RegLs:
        return RunRegisterLs(lsPattern, verbose);
    case Action::RegRm:
        return RunRegisterRm(rmRegisterName);
    case Action::Gui:
    case Action::None:
        break;
    }
    return std::nullopt;
}

}  // namespace clipp::cli
