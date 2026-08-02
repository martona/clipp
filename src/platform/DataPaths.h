#pragma once

#include <string>

namespace clipp {

// Per-user directories for everything Clipp writes beside its settings store.
// Output is UTF-8; every function returns false when the location can't be
// determined (or, where noted, created). Only the Windows and macOS targets
// compile implementations — the headless Linux CLI writes nothing here and
// iOS gets its own container plumbing later — so callers guard with
// `#if defined(_WIN32) || defined(__APPLE__)`.

// App state files (the sealed register snapshot lives here), created:
//   * Windows -> %LOCALAPPDATA%\Clipp\state  (the REAL location even under
//                MSIX: the package disables file-system write virtualization
//                so the SendTo shortcut can reach the real shell:SendTo folder
//                — see AppxManifest.xml.in. Settings/key/host id are registry
//                and stay package-virtualized.)
//   * macOS   -> ~/Library/Application Support/Clipp  (container-mapped under
//                the MAS sandbox)
bool ResolveStateDirectory(std::string& outUtf8Dir);

// Rolling log files:
//   * Windows -> %LOCALAPPDATA%\Clipp\logs   (NOT created here — the logger
//                creates the leaf lazily on the first emitted line)
//   * macOS   -> ~/Library/Logs/Clipp        (created; surfaces in Console.app,
//                container-mapped under the MAS sandbox)
bool ResolveLogDirectory(std::string& outUtf8Dir);

// Minidumps, created: %LOCALAPPDATA%\Clipp\crashdumps. Windows-only — no
// macOS implementation exists (the OS crash reporter owns that side). The
// crash handler resolves this ONCE at install time into a fixed buffer; the
// actual crash path never calls back in here (no allocation mid-crash).
bool ResolveCrashDumpDirectory(std::string& outUtf8Dir);

}  // namespace clipp
