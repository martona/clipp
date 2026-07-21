#pragma once

// Is this a build that runs the named-register daemon — i.e. defines
// g_registerStore, serves register frames, and advertises CAP0_SERVES_REGISTERS?
//
// Only the desktop GUI builds (Windows, macOS) do. The headless Linux CLI runs no
// daemon at all, and the iOS app's register wiring is deferred (Peer.cpp and
// CryptoChannel.cpp compile on iOS, but g_registerStore is defined only in the
// desktop main.cpp). Gate all daemon-side register code on this so the iOS and
// headless builds compile and stay non-participating.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(CLIPP_HEADLESS)
#define CLIPP_REGISTERS_DAEMON 0
#elif defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
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
// shared CryptoChannel is moot — see the LocalCaps comment.
#if defined(CLIPP_HEADLESS)
#define CLIPP_SERVES_NETMAP 0
#else
#define CLIPP_SERVES_NETMAP 1
#endif
