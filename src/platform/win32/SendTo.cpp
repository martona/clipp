#include "SendTo.h"

#include "ClipboardFormat.h"
#include "ClipboardLimits.h"
#include "ClipboardPayload.h"
#include "CryptoChannel.h"
#include "HostId.h"
#include "KeyManager.h"
#include "LocalPeerName.h"
#include "Logger.h"
#include "OneShotPeer.h"
#include "Settings.h"
#include "SodiumInit.h"
#include "platform/uistrings.h"
#include "clipp-win32-darkmode32/DMSubclass.h"

#include <Windows.h>
#include <appmodel.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {
    constexpr wchar_t kSendToLinkFileName[] = L"\\Clipp.lnk";
    constexpr wchar_t kSendToArgument[] = L"--sendto";
    constexpr DWORD kMaxModulePathLength = 32768;

    std::wstring GetCurrentExecutablePath() {
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

    // Full path of the Clipp.lnk inside the per-user SendTo folder. Under MSIX this
    // resolves to the REAL shell folder and the write actually lands there because
    // the package disables file-system write virtualization (see AppxManifest.xml.in);
    // without that the .lnk would be redirected into the package overlay and Explorer
    // would never see it — the same trap that made the HKCU Run key inert.
    std::wstring ResolveSendToLinkPath() {
        PWSTR sendTo = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(FOLDERID_SendTo, KF_FLAG_DEFAULT, nullptr, &sendTo);
        if (FAILED(hr) || sendTo == nullptr) {
            if (sendTo != nullptr) {
                CoTaskMemFree(sendTo);
            }
            return {};
        }
        std::wstring path(sendTo);
        CoTaskMemFree(sendTo);
        path += kSendToLinkFileName;
        return path;
    }

    // Stable .lnk target when packaged. The versioned WindowsApps path
    // (\WindowsApps\<name>_<version>_<arch>__<hash>\clippmain.exe) dies on every
    // package update, and the .lnk only self-heals at the NEXT GUI launch — a dead
    // SendTo entry in between. The App Execution Alias declared in the manifest
    // (%LOCALAPPDATA%\Microsoft\WindowsApps\clippgui.exe) is version-stable,
    // user-readable, forwards arguments, and launches with package identity.
    // Empty when unpackaged, or when the alias is missing (older package, or the
    // user toggled it off under Settings > Apps > App execution aliases) — the
    // caller then falls back to the real exe path.
    std::wstring ResolvePackagedAliasPath() {
        UINT32 length = 0;
        if (::GetCurrentPackageFamilyName(&length, nullptr) == APPMODEL_ERROR_NO_PACKAGE) {
            return {};
        }
        PWSTR localAppData = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData);
        if (FAILED(hr) || localAppData == nullptr) {
            if (localAppData != nullptr) {
                CoTaskMemFree(localAppData);
            }
            return {};
        }
        std::wstring path(localAppData);
        CoTaskMemFree(localAppData);
        path += L"\\Microsoft\\WindowsApps\\clippgui.exe";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return {};
        }
        return path;
    }

    // Registration runs early on the main thread, before the tray sets up any
    // apartment; the handler path never touches COM. Balance whatever we start.
    // RPC_E_CHANGED_MODE still leaves a usable apartment for IShellLink.
    struct ComApartment {
        HRESULT hr;
        ComApartment() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
        ~ComApartment() {
            if (SUCCEEDED(hr)) {
                CoUninitialize();
            }
        }
        bool Usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
    };
}

bool RegisterClippSendTo() {
    const std::wstring linkPath = ResolveSendToLinkPath();
    const std::wstring exePath = GetCurrentExecutablePath();
    if (linkPath.empty() || exePath.empty()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to resolve SendTo link or executable path.");
        return false;
    }

    ComApartment com;
    if (!com.Usable()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"COM initialization failed for SendTo registration (hr=0x%08X).", com.hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to create shell link instance (hr=0x%08X).", hr);
        return false;
    }
    std::wstring targetPath = ResolvePackagedAliasPath();
    if (targetPath.empty()) {
        targetPath = exePath;
    }
    link->SetPath(targetPath.c_str());
    // Explorer appends the selected file paths after these arguments.
    link->SetArguments(kSendToArgument);
    // Icon from the real exe even when the target is the alias (a 0-byte reparse
    // point with nothing to extract); refreshed every launch, so a post-update
    // stale icon path heals itself.
    link->SetIconLocation(exePath.c_str(), 0);
    link->SetDescription(CLP_W(CLP_UI_SENDTO_DESCRIPTION));

    Microsoft::WRL::ComPtr<IPersistFile> file;
    hr = link.As(&file);
    if (SUCCEEDED(hr)) {
        hr = file->Save(linkPath.c_str(), TRUE);
    }
    if (FAILED(hr)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to save SendTo link (hr=0x%08X).", hr);
        return false;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Registered SendTo link: %ls -> %ls", linkPath.c_str(), targetPath.c_str());
    return true;
}

bool UnregisterClippSendTo() {
    const std::wstring linkPath = ResolveSendToLinkPath();
    if (linkPath.empty()) {
        return false;
    }
    if (DeleteFileW(linkPath.c_str()) == 0) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to remove SendTo link (GetLastError=%lu).", error);
            return false;
        }
    }
    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Unregistered SendTo link.");
    return true;
}

// ---------------------------------------------------------------------------
// --sendto invocation: build one payload per file and push the batch through a
// gateway peer exactly like `clipp copy` does. The gateway applies each item
// locally (OS clipboard + history) and rebroadcasts it to the mesh, in order —
// so every file lands in the clipboard history everywhere and the LAST file is
// what remains on the live clipboard.
// ---------------------------------------------------------------------------

namespace {
    struct WsaSession {
        bool ok;
        WsaSession() {
            WSADATA data;
            ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }
        ~WsaSession() {
            if (ok) {
                WSACleanup();
            }
        }
    };

    void ShowSendToError(const std::wstring& text) {
        DarkMode::DarkMessageBox(nullptr, text.c_str(), CLP_W(CLP_UI_APP_NAME), MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }

    std::wstring FileNameOf(const std::wstring& path) {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& out, std::wstring& error) {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = CLP_W(CLP_UI_SENDTO_ERR_UNREADABLE);
            return false;
        }
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) == 0 || size.QuadPart < 0) {
            CloseHandle(file);
            error = CLP_W(CLP_UI_SENDTO_ERR_UNREADABLE);
            return false;
        }
        if (static_cast<unsigned long long>(size.QuadPart) > ClipboardLimits::kMaxDecompressedClipboardBytes) {
            CloseHandle(file);
            error = CLP_W(CLP_UI_SENDTO_ERR_TOO_LARGE);
            return false;
        }
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t offset = 0;
        while (offset < out.size()) {
            const DWORD chunk = static_cast<DWORD>(
                std::min<size_t>(out.size() - offset, 1u << 20));
            DWORD read = 0;
            if (ReadFile(file, out.data() + offset, chunk, &read, nullptr) == 0 || read == 0) {
                CloseHandle(file);
                error = CLP_W(CLP_UI_SENDTO_ERR_UNREADABLE);
                return false;
            }
            offset += read;
        }
        CloseHandle(file);
        return true;
    }

    bool Utf16ToUtf8Bytes(const wchar_t* data, size_t chars, std::vector<unsigned char>& out) {
        if (chars == 0) {
            out.clear();
            return true;
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(chars),
                                               nullptr, 0, nullptr, nullptr);
        if (needed <= 0) {
            return false;
        }
        out.resize(static_cast<size_t>(needed));
        return WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(chars),
                                   reinterpret_cast<char*>(out.data()), needed, nullptr, nullptr) == needed;
    }

    // Content-sniffed, not extension-driven: the SendTo menu appears for EVERY file
    // type (Explorer offers no way to scope it), so the handler must decide from the
    // bytes. Images pass through as-is (the wire formats are PNG/JPEG containers);
    // everything else must be text — UTF-8, or UTF-16 with a BOM (Notepad's other
    // default), which is recoded. Anything else is refused per-file.
    bool BuildPayloadFromFile(const std::wstring& path, ClipboardPayload& payload, std::wstring& error) {
        std::vector<unsigned char> bytes;
        if (!ReadFileBytes(path, bytes, error)) {
            return false;
        }
        if (bytes.empty()) {
            error = CLP_W(CLP_UI_SENDTO_ERR_EMPTY);
            return false;
        }

        static constexpr unsigned char kPngMagic[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (bytes.size() >= sizeof(kPngMagic) && std::memcmp(bytes.data(), kPngMagic, sizeof(kPngMagic)) == 0) {
            payload.meta.formatId = CLIPP_FORMAT_PNG;
        } else if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
            payload.meta.formatId = CLIPP_FORMAT_JPEG;
        } else {
            // UTF-16 BOM (LE or BE): recode to the UTF-8 wire form.
            if (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF))) {
                if ((bytes.size() % 2) != 0) {
                    error = CLP_W(CLP_UI_SENDTO_ERR_NOT_TEXT);
                    return false;
                }
                const bool bigEndian = bytes[0] == 0xFE;
                std::wstring wide(reinterpret_cast<const wchar_t*>(bytes.data()) + 1, bytes.size() / 2 - 1);
                if (bigEndian) {
                    for (wchar_t& c : wide) {
                        c = static_cast<wchar_t>((static_cast<unsigned>(c) >> 8) | ((c & 0xFF) << 8));
                    }
                }
                if (!Utf16ToUtf8Bytes(wide.data(), wide.size(), bytes)) {
                    error = CLP_W(CLP_UI_SENDTO_ERR_NOT_TEXT);
                    return false;
                }
            } else if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
                bytes.erase(bytes.begin(), bytes.begin() + 3);
            }
            if (bytes.empty()) {
                error = CLP_W(CLP_UI_SENDTO_ERR_EMPTY);
                return false;
            }
            // An embedded NUL means binary (and would truncate at paste); reject.
            // MB_ERR_INVALID_CHARS rejects malformed sequences but not NUL itself.
            if (std::memchr(bytes.data(), 0, bytes.size()) != nullptr
                || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       reinterpret_cast<const char*>(bytes.data()),
                                       static_cast<int>(bytes.size()), nullptr, 0) <= 0) {
                error = CLP_W(CLP_UI_SENDTO_ERR_NOT_TEXT);
                return false;
            }
            // Trailing NUL matches the platform capture convention (receivers strip
            // one when writing to the clipboard); SetUncompressedBytes canonicalizes
            // CRLF -> LF for the platform-neutral wire form.
            bytes.push_back('\0');
            payload.meta.formatId = CLIPP_FORMAT_UTF8;
        }

        if (!payload.SetUncompressedBytes(std::move(bytes))) {
            error = CLP_W(CLP_UI_SENDTO_ERR_ENCODE);
            return false;
        }
        return true;
    }

    int RunSendTo(const std::vector<std::wstring>& files) {
        // The GUI's tray loop normally does this; the helper exits long before it,
        // so derive the OS light/dark mode here or every DarkMessageBox below
        // renders light. (_DARKMODE_NO_INI_CONFIG build: follows the registry.)
        DarkMode::initDarkMode();
        if (files.empty()) {
            ShowSendToError(CLP_W(CLP_UI_SENDTO_ERR_NO_FILES));
            return 1;
        }
        if (!InitializeSodium()) {
            ShowSendToError(CLP_W(CLP_UI_SENDTO_ERR_STARTUP));
            return 1;
        }
        WsaSession wsa;
        if (!wsa.ok) {
            ShowSendToError(CLP_W(CLP_UI_SENDTO_ERR_STARTUP));
            return 1;
        }
        if (!g_keyManager.HaveNetworkKey()) {
            ShowSendToError(CLP_W(CLP_UI_SENDTO_ERR_NOT_PAIRED));
            return 1;
        }

        // ensure (not get): stamp a real host ID even if the GUI never ran (see
        // RunCopy — an all-zero id would poison origin identity on the wire).
        HostId localHostId;
        if (!g_settings.ensureHostID(localHostId)) {
            ShowSendToError(CLP_W(CLP_UI_SENDTO_ERR_STARTUP));
            return 1;
        }
        const std::string localHostName = clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);

        std::vector<ClipboardPayload> payloads;
        std::wstring fileErrors;
        for (const std::wstring& file : files) {
            ClipboardPayload payload;
            std::wstring error;
            if (BuildPayloadFromFile(file, payload, error)) {
                payload.StampOrigin(localHostId, localHostName.c_str(), g_settings.nextOriginSequenceNumber());
                payloads.push_back(std::move(payload));
            } else {
                fileErrors += FileNameOf(file) + L": " + error + L"\n";
            }
        }

        // Relay whatever was usable even if some files were refused — a mixed
        // selection shouldn't hold the good files hostage.
        if (!payloads.empty()) {
            const size_t relayedCount = payloads.size();
            const auto via = OneShot::RelayPayloads(std::move(payloads), localHostId, localHostName,
                                                    /*includeSelf=*/true);
            if (!via) {
                ShowSendToError(fileErrors + CLP_W(CLP_UI_SENDTO_ERR_NO_GATEWAY));
                return 1;
            }
            g_logger.log(__FUNCTION__, Logger::Level::Info, L"SendTo relayed %zu file(s) via %hs.",
                relayedCount, via->deviceName.c_str());
        }

        if (!fileErrors.empty()) {
            ShowSendToError(fileErrors);
            return 1;
        }
        return 0;
    }
}

std::optional<int> RunSendToIfRequested() {
    // main()'s argv is ANSI-lossy for paths outside the active code page; always
    // re-parse the wide command line.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return std::nullopt;
    }
    const bool isSendTo = argc >= 2 && _wcsicmp(argv[1], kSendToArgument) == 0;
    std::vector<std::wstring> files;
    if (isSendTo) {
        files.assign(argv + 2, argv + argc);
    }
    LocalFree(argv);
    if (!isSendTo) {
        return std::nullopt;
    }
    return RunSendTo(files);
}
