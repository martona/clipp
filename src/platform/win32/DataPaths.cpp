#include "platform/DataPaths.h"

#ifdef _WIN32

#include "platform.h"  // clipp_platform_detail::Utf16ToUtf8String
#include <shlobj.h>

#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace clipp {

namespace {

// %LOCALAPPDATA%\Clipp\<leaf>, optionally created (with intermediates).
bool ResolveClippSubdirectory(const wchar_t* leaf, bool create, std::string& outUtf8Dir) {
    PWSTR localAppData = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    if (FAILED(hr) || localAppData == nullptr) {
        if (localAppData != nullptr) {
            CoTaskMemFree(localAppData);
        }
        return false;
    }

    std::wstring dir(localAppData);
    CoTaskMemFree(localAppData);
    dir.append(L"\\Clipp\\").append(leaf);

    if (create) {
        const int result = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS
            && result != ERROR_FILE_EXISTS) {
            return false;
        }
    }

    outUtf8Dir = clipp_platform_detail::Utf16ToUtf8String(dir);
    return !outUtf8Dir.empty();
}

}  // namespace

bool ResolveStateDirectory(std::string& outUtf8Dir) {
    return ResolveClippSubdirectory(L"state", /*create=*/true, outUtf8Dir);
}

bool ResolveLogDirectory(std::string& outUtf8Dir) {
    // The logger creates the leaf lazily on the first emitted line.
    return ResolveClippSubdirectory(L"logs", /*create=*/false, outUtf8Dir);
}

bool ResolveCrashDumpDirectory(std::string& outUtf8Dir) {
    return ResolveClippSubdirectory(L"crashdumps", /*create=*/true, outUtf8Dir);
}

}  // namespace clipp

#endif  // _WIN32
