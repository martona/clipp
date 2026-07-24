// Register persistence on iOS: reuses the desktop's sealed-snapshot module AND
// its daemon wiring WHOLESALE. RegisterPersistence.cpp is pure portable C++
// (POSIX fopen/fsync/rename on this branch), and RegisterPersistenceRuntime.cpp
// needs exactly one platform service — clipp::ResolveStateDirectory — which this
// file provides for the app container. Same include-the-cpp pattern as the other
// *Core.mm bridges.
//
// This file rides the app target's file-system-synchronized group; the share
// extension (explicit source membership) never compiles it, so the extension
// can't even name the snapshot path — the "ext must never read it" rule is
// enforced at link time, not by discipline.
#import <Foundation/Foundation.h>

#include "../../../src/RegisterConfig.h"

#if CLIPP_REGISTERS_DAEMON

#include "../../../src/platform/DataPaths.h"

#include <string>

namespace clipp {

// iOS flavor of platform/DataPaths (win32/DataPaths.cpp | macos/DataPaths.mm):
// state lives in the APP CONTAINER's Application Support — deliberately NOT the
// app group, so the share extension has no path to the sealed snapshot. Two
// attributes are applied to the directory:
//   * NSURLIsExcludedFromBackupKey — the registers are mesh-replicated state; a
//     device restore re-seeds from peers (or from this device's own re-derived
//     key + file next launch), and an iCloud/iTunes backup must not carry the
//     sealed blob to other contexts. Re-asserted every resolve (cheap, and the
//     flag is documented to be restorable-lossy).
//   * NSFileProtectionCompleteUntilFirstUserAuthentication — files created
//     inside a directory inherit its protection class, so the shared C++
//     temp+fsync+rename write path needs no per-file attribute calls. This
//     matches the iOS default for third-party apps; setting it explicitly
//     documents the requirement (writes must work while backgrounded/locked,
//     which rules out the stricter NSFileProtectionComplete).
bool ResolveStateDirectory(std::string& outUtf8Dir) {
    @autoreleasepool {
        NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(
            NSApplicationSupportDirectory, NSUserDomainMask, YES);
        if (paths.count == 0) {
            return false;
        }
        NSString* dir = [paths.firstObject stringByAppendingPathComponent:@"Clipp"];
        NSDictionary* attributes = @{
            NSFileProtectionKey : NSFileProtectionCompleteUntilFirstUserAuthentication,
        };
        NSError* error = nil;
        if (![NSFileManager.defaultManager createDirectoryAtPath:dir
                                     withIntermediateDirectories:YES
                                                      attributes:attributes
                                                           error:&error]) {
            return false;
        }
        NSURL* url = [NSURL fileURLWithPath:dir isDirectory:YES];
        [url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
        const char* utf8 = dir.UTF8String;
        if (utf8 == nullptr) {
            return false;
        }
        outUtf8Dir = utf8;
        return true;
    }
}

}  // namespace clipp

#include "../../../src/RegisterPersistence.cpp"
#include "../../../src/RegisterPersistenceRuntime.cpp"

#endif  // CLIPP_REGISTERS_DAEMON
