#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Shared Swift-facing API. Each Apple app implements this using its existing
// history-copy action; clipboard content never becomes a Shortcuts result.
NS_SWIFT_NAME(AppleShortcutsBridge)
@interface CLPAppleShortcutsBridge : NSObject

+ (BOOL)copyLatestItemWithError:(NSError**)error NS_SWIFT_NAME(copyLatestItem());

@end

NS_ASSUME_NONNULL_END

#ifdef __cplusplus
extern "C" {
#endif
// Implemented in the shared Swift source; called on the macOS main thread.
void CLPRegisterAppleShortcuts(void);
#ifdef __cplusplus
}
#endif
