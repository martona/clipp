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
