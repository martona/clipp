// Compiles the named-register engine into the iOS app target. The desktop keeps
// the g_registerStore definition in main.cpp and the engine .cpp files as regular
// CMake translation units; iOS uses the same include-the-cpp pattern as the other
// *Core.mm bridges (ClippPeerCore.mm owns Peer.cpp, etc.), so this file OWNS the
// single definition of g_registerStore plus the two pure engine TUs it needs.
//
// This file rides the app target's file-system-synchronized group. The share
// extension has explicit (non-synchronized) source membership and does NOT compile
// this file, so it never references g_registerStore — it only over-advertises the
// register capability harmlessly through the shared CryptoChannel (see
// RegisterConfig.h). Keeping the .cpp includes here (and nowhere else on iOS)
// guarantees exactly one definition of every RegisterStore/RegisterWire symbol;
// Peer.cpp (in ClippPeerCore.mm) resolves them across TUs at link time.
#include "../../../src/RegisterConfig.h"

#if CLIPP_REGISTERS_DAEMON

#include "../../../src/RegisterStore.h"

// The daemon-global the header declares extern (defined in main.cpp on desktop).
RegisterStore g_registerStore;

// Hlc.cpp was never needed on iOS before registers (nothing referenced the HLC
// clock); the register engine and Peer.cpp's anti-entropy pull it in now. Compiled
// here, once, alongside the engine it serves.
#include "../../../src/Hlc.cpp"
#include "../../../src/RegisterStore.cpp"
#include "../../../src/RegisterWire.cpp"

#endif  // CLIPP_REGISTERS_DAEMON
