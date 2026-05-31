#include "platform/LogPaths.h"

#ifdef _WIN32

#include <windows.h>
#include <shlobj.h>

#include <string>

#include "platform.h"  // clipp_platform_detail::Utf16ToUtf8String

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace clipp {

bool ResolveLogDirectory(std::string& outUtf8Dir) {
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

    // Sibling of the crash-dump directory (see win32/CrashHandler.cpp). The logger
    // creates the leaf lazily on the first emitted line.
    dir.append(L"\\Clipp\\logs");

    outUtf8Dir = clipp_platform_detail::Utf16ToUtf8String(dir);
    return !outUtf8Dir.empty();
}

} // namespace clipp

#endif // _WIN32
