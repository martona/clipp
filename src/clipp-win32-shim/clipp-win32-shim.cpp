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

    // 2. Get the full path of the shim (e.g., C:\path\clipp.com, or C:\path\clipp.exe
    //    when packaged for MSIX, where the shim itself is renamed clipp.exe).
    wchar_t pathBuffer[MAX_PATH]{};
    GetModuleFileNameW(NULL, pathBuffer, MAX_PATH);
    std::wstring selfPath(pathBuffer);

    // 3. Resolve the target to launch.
    //    Non-packaged: the shim is clipp.com and launches its sibling clipp.exe (the GUI).
    //    Packaged (MSIX): MSIX requires the alias target to end in .exe, so the shim is
    //    itself clipp.exe and the GUI is renamed clippmain.exe. If our own name already
    //    ends in .exe, launch clippmain.exe in the same folder -- otherwise the swap
    //    below would point us back at ourselves.
    size_t extPos = selfPath.find_last_of(L'.');
    bool selfIsExe = (extPos != std::wstring::npos) &&
                     (lstrcmpiW(selfPath.c_str() + extPos, L".exe") == 0);

    std::wstring exePath;
    if (selfIsExe) {
        size_t slashPos = selfPath.find_last_of(L"\\/");
        std::wstring dir = (slashPos != std::wstring::npos)
            ? selfPath.substr(0, slashPos + 1)
            : std::wstring();
        exePath = dir + L"clippmain.exe";
    } else if (extPos != std::wstring::npos) {
        exePath = selfPath.substr(0, extPos) + L".exe";
    } else {
        exePath = selfPath + L".exe";
    }

    // 4. Pass along the exact command line arguments the user typed
    wchar_t* cmdLine = GetCommandLineW();

    // 5. Launch the actual GUI application.
    //    Forward our standard handles to the child so shell redirection and pipes
    //    (clipp paste > file, echo x | clipp copy) actually reach it. The child is
    //    a GUI-subsystem binary with no console of its own; without this it falls
    //    back to attaching our console and loses any redirection. STARTF_USESTDHANDLES
    //    requires the handles to be inheritable and bInheritHandles = TRUE.
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    HANDLE stdHandles[3] = {
        GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE),
    };
    for (HANDLE h : stdHandles) {
        if (h && h != INVALID_HANDLE_VALUE) {
            SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
    }
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = stdHandles[0];
    si.hStdOutput = stdHandles[1];
    si.hStdError = stdHandles[2];

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(
        exePath.c_str(),
        cmdLine,
        NULL,
        NULL,
        TRUE,   // bInheritHandles: required for the STARTF_USESTDHANDLES forwarding above
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