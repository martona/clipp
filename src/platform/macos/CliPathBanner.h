#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

// A dismissible banner that nudges the user to put the bundled `clipp` CLI on their
// PATH. Lives in the main window's shell (above the swapped page content) so it shows
// on every page. macOS-only and GUI-only.
//
// Visibility is the controller's job: call +shouldShow before creating/showing it.
// The banner advertises a Terminal command (it cannot create the symlink itself --
// the Mac App Store sandbox forbids writing outside the container, and we keep the
// direct-download flow identical), with a Copy button and a permanent Dismiss.
@interface ClippCliPathBanner : NSView

// Whether the banner is currently warranted:
//   * the user has connected to a peer at least once (the sticky latch below) -- proof
//     they are genuinely past initial setup, not just mid-configuration, AND
//   * the user has not permanently dismissed it, AND
//   * the bundled clipp is not already symlinked into a common PATH location.
// Cheap (a couple of stat()s + NSUserDefaults reads); safe to call on the main thread.
// Intended to be polled (the host re-evaluates on a timer while the window is open).
+ (BOOL)shouldShow;

// Records that the user has had at least one live peer connection. Sticky: once set,
// it stays set (persisted in NSUserDefaults), so the banner keeps offering the CLI
// even when peers are later offline. Call whenever the live connected-peer count is
// > 0; cheap and idempotent once latched.
+ (void)noteConnectedPeers;

// YES when the banner's outcome is decided for good and no amount of polling could
// bring it back: the user has dismissed it, or the CLI is already installed. NOT
// resolved merely because no peer has connected yet -- that's the transient state the
// poll timer is waiting on, so callers must keep polling then. Lets the host avoid
// arming (or promptly stop) the 1s timer once there's nothing left to watch. The
// "installed" half is re-checked on each window show, so the absurd case of deleting
// the symlink mid-session simply resurfaces the banner on the next show.
+ (BOOL)isResolved;

// Marks the banner permanently dismissed (persisted in NSUserDefaults). After this,
// +shouldShow returns NO for good. Exposed so callers can reset it in tests/dev.
+ (void)markDismissed;

// dismissHandler is invoked (on the main thread) when the user clicks Dismiss, after
// the dismissal has been persisted. The controller uses it to collapse/remove the
// banner from the shell layout.
- (instancetype)initWithDismissHandler:(void (^)(void))dismissHandler;

@end

#endif  // __APPLE__
