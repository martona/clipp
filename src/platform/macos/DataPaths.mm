#include "platform/DataPaths.h"

#import <Foundation/Foundation.h>

namespace clipp {

bool ResolveLogDirectory(std::string& outUtf8Dir) {
    @autoreleasepool {
        // ~/Library/Logs/Clipp -- Apple's blessed location for app logs (it
        // surfaces in Console.app). Resolved via the search API rather than
        // hardcoding ~/Library so the MAS sandbox build transparently lands in
        // the container's Data/Library/Logs.
        NSArray<NSString*>* libraryPaths =
            NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES);
        if (libraryPaths.count == 0) {
            return false;
        }

        NSString* logsDir = [[libraryPaths.firstObject
            stringByAppendingPathComponent:@"Logs"]
            stringByAppendingPathComponent:@"Clipp"];

        NSError* error = nil;
        const BOOL created = [[NSFileManager defaultManager]
                  createDirectoryAtPath:logsDir
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:&error];
        if (!created) {
            return false;
        }

        const char* utf8 = [logsDir fileSystemRepresentation];
        if (utf8 == nullptr) {
            return false;
        }
        outUtf8Dir = utf8;
        return true;
    }
}

bool ResolveStateDirectory(std::string& outUtf8Dir) {
    @autoreleasepool {
        // ~/Library/Application Support/Clipp — resolved via the search API
        // rather than hardcoding ~/Library so the MAS sandbox build
        // transparently lands in the container's Data/Library/Application
        // Support (the /tmp lesson: never assume a path shape under sandbox).
        NSArray<NSString*>* supportPaths = NSSearchPathForDirectoriesInDomains(
            NSApplicationSupportDirectory, NSUserDomainMask, YES);
        if (supportPaths.count == 0) {
            return false;
        }

        NSString* stateDir =
            [supportPaths.firstObject stringByAppendingPathComponent:@"Clipp"];

        NSError* error = nil;
        const BOOL created = [[NSFileManager defaultManager]
                  createDirectoryAtPath:stateDir
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:&error];
        if (!created) {
            return false;
        }

        const char* utf8 = [stateDir fileSystemRepresentation];
        if (utf8 == nullptr) {
            return false;
        }
        outUtf8Dir = utf8;
        return true;
    }
}

}  // namespace clipp
