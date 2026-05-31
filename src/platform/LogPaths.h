#pragma once

#include <string>

namespace clipp {

// Resolves the per-user directory where rolling log files are written, creating it
// if needed. Output is UTF-8.
//
// Only the Windows and macOS targets compile an implementation:
//   * Windows -> %LOCALAPPDATA%\Clipp\logs   (sibling of \Clipp\crashdumps)
//   * macOS   -> ~/Library/Logs/Clipp        (container-mapped under the MAS sandbox)
// The Linux / headless CLI logs to stderr by design and iOS has no desktop-style log
// directory, so those builds neither provide an implementation nor reference this
// symbol (callers must guard with `#if defined(_WIN32) || defined(__APPLE__)`).
//
// Returns false if the location can't be determined or created.
bool ResolveLogDirectory(std::string& outUtf8Dir);

} // namespace clipp
