#include "platform/DataPaths.h"

#ifdef _WIN32

#include "platform.h"  // clipp_platform_detail::Utf16ToUtf8String
#include <shlobj.h>

#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace clipp {

bool ResolveStateDirectory(std::string& outUtf8Dir) {
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

    // Sibling of \Clipp\logs and \Clipp\crashdumps. Unlike the log dir (whose
    // leaf the logger creates lazily), callers here write immediately — create
    // the whole chain now.
    dir.append(L"\\Clipp\\state");
    const int created = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS
        && created != ERROR_FILE_EXISTS) {
        return false;
    }

    outUtf8Dir = clipp_platform_detail::Utf16ToUtf8String(dir);
    return !outUtf8Dir.empty();
}

}  // namespace clipp

#endif  // _WIN32
