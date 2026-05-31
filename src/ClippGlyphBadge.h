#pragma once

#ifdef __APPLE__

#include "OsType.h"

#import <CoreGraphics/CoreGraphics.h>

// Renders the compound device mark for `osType` -- the OS-family glyph plus a smaller
// device-type badge with a background-color knockout halo -- into `ctx`, sized and
// centered within `bounds`.
//
// Pure CoreText/CoreGraphics, so macOS (AppKit) and iOS (UIKit) share it verbatim:
// colors are CGColorRef (pass NSColor.CGColor / UIColor.CGColor). The context must be
// y-up: AppKit's non-flipped NSView is already y-up; a UIKit drawRect: must flip
// (translate by height, scale y by -1) before calling.
void ClippDrawDeviceBadge(CGContextRef ctx,
                          CGRect bounds,
                          OsType osType,
                          CGColorRef familyColor,
                          CGColorRef deviceColor,
                          CGColorRef haloColor);

#endif  // __APPLE__
