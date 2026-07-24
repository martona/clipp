#pragma once

// Is this a build that runs the named-register daemon — i.e. defines
// g_registerStore, serves register frames, and advertises CAP0_SERVES_REGISTERS?
//
// The desktop GUI builds (Windows, macOS) AND the iOS app do: the iOS app runs a
// foreground runtime with a listener, defines g_registerStore (ClippRegisterCore.mm),
// and compiles the register engine (RegisterStore.cpp / RegisterWire.cpp) + Peer.cpp's
// REGW/RSYN anti-entropy the same way the desktop does. The headless Linux CLI runs no
// daemon at all. The share extension compiles no Peer.cpp and never accepts inbound, so
// the CAP0_SERVES_REGISTERS it (over-)advertises via the shared CryptoChannel is moot —
// exactly like CAP0_SERVES_NETMAP below (RemoteServesRegisters() gates nothing, and the
// register-sync push is driven by the connecting peer's own gate, not by our caps). Gate
// all daemon-side register code on this so the headless build compiles non-participating.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(CLIPP_NO_DAEMON)
#define CLIPP_REGISTERS_DAEMON 0
#else
#define CLIPP_REGISTERS_DAEMON 1
#endif

// NMAP (`clipp map`) serving is a WIDER population than registers: it needs a
// listener plus the g_peerDisplay connection table, which the desktop daemons
// AND the iOS app have (the iOS bridge defines g_peerDisplay and runs the
// listener; foregrounded it answers, suspended it shows as unreachable — both
// honest). The headless CLI runs no daemon. The share extension compiles no
// Peer.cpp and never accepts inbound, so the cap it (over-)advertises via the
// shared CryptoChannel is moot — see the LocalCaps comment. The no-daemon builds
// (headless Linux CLI + Windows clipp.com companion) run no listener at all.
#if defined(CLIPP_NO_DAEMON)
#define CLIPP_SERVES_NETMAP 0
#else
#define CLIPP_SERVES_NETMAP 1
#endif
