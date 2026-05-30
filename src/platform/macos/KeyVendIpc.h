#pragma once

// macOS-only local IPC that lets a command-line invocation of clipp obtain the
// root network key from the running GUI when the login keychain is unreachable
// (e.g. an SSH / headless session). Transport is an AF_UNIX socket in the app's
// container/Application Support dir; both ends mutually authenticate by checking
// the peer's audit token against this binary's own designated requirement, so
// only the same signed clipp can serve or receive the key.
//
// Compiled only in the macOS desktop target (never iOS — the share extension uses
// keychain access groups directly).

#include "../../KeyManager.h"  // KeyManager::NetworkKey / NetworkKeySize

#include <string>

namespace clipp::macos {

// GUI side. Starts the key-vending listener on a background thread (idempotent).
// Also marks this process as the server so KeyManager's keychain-failure fallback
// never dials this same process. Safe no-op if it can't bind.
void StartKeyVendServer();

// GUI side. Stops the listener, joins its thread, and removes the socket.
void StopKeyVendServer();

// True while this process is running the vend server (the GUI). KeyManager checks
// this to skip the socket fallback in the server process itself.
bool IsKeyVendServerActive();

// CLI side. Connects to the running GUI's socket, mutually authenticates, and
// fetches the 32-byte root network key into outKey. Returns false (with
// *errorMessage, if provided) when the GUI isn't running, authentication fails,
// the GUI has no key, or the transfer fails.
bool RequestNetworkKeyOverSocket(KeyManager::NetworkKey& outKey, std::string* errorMessage);

}  // namespace clipp::macos
