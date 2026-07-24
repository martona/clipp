#pragma once

#include <string>

namespace clipp {

// Resolves the per-user directory for app state files (the sealed register
// snapshot lives here), creating it if needed. Output is UTF-8.
//
// Only the Windows and macOS targets compile an implementation (same contract
// as LogPaths.h — headless Linux runs no daemon/store and iOS gets its own
// container plumbing later):
//   * Windows -> %LOCALAPPDATA%\Clipp\state  (sibling of \logs and
//                \crashdumps; under MSIX the write lands in the package's
//                virtualized LocalAppData, which is exactly the scoping we want)
//   * macOS   -> ~/Library/Application Support/Clipp  (container-mapped under
//                the MAS sandbox)
//
// Returns false if the location can't be determined or created.
bool ResolveStateDirectory(std::string& outUtf8Dir);

}  // namespace clipp
