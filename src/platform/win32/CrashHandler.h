#pragma once

namespace clipp {

// Installs unhandled-exception, terminate, pure-virtual, and invalid-CRT-
// parameter handlers that write a minidump to %LOCALAPPDATA%\Clipp\crashdumps\
// when the process is about to die, then let the process terminate.
//
// Safe to call once early in main(). No-ops on subsequent calls. Off-Windows
// callers should just not include this header.
void InstallCrashHandler();

} // namespace clipp
