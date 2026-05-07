#include <windows.h>
#include <string>

// The shim must completely ignore all console signals.
// It will only die when the child .exe process actually exits.
BOOL WINAPI IgnoreSignalsHandler(DWORD dwCtrlType) {
    return TRUE;
}

int wmain(int argc, wchar_t* argv[]) {
    // 1. Hook the signal handler to act as a shield
    SetConsoleCtrlHandler(IgnoreSignalsHandler, TRUE);

    // 2. Get the full path of the shim (e.g., C:\path\clipp.com)
    wchar_t pathBuffer[MAX_PATH]{};
    GetModuleFileNameW(NULL, pathBuffer, MAX_PATH);
    std::wstring exePath(pathBuffer);

    // 3. Swap the .com extension for .exe
    size_t extPos = exePath.find_last_of(L".");
    if (extPos != std::wstring::npos) {
        exePath = exePath.substr(0, extPos) + L".exe";
    } else {
        exePath += L".exe";
    }

    // 4. Pass along the exact command line arguments the user typed
    wchar_t* cmdLine = GetCommandLineW();

    // 5. Launch the actual GUI application
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(
        exePath.c_str(),
        cmdLine,
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    )) {
        // If the .exe is missing, just bail.
        return 1;
    }

    // 6. Sleep and wait for the child .exe to finish its graceful shutdown.
    WaitForSingleObject(pi.hProcess, INFINITE);

    // 7. Pass the child's exit code back to cmd.exe
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode;
}