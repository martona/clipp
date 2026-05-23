#import <Foundation/Foundation.h>

#include "../../../src/platform/uistrings.h"

NS_ASSUME_NONNULL_BEGIN

NS_SWIFT_NAME(NetworkKeyStatus)
@interface CLPNetworkKeyStatus : NSObject

@property(nonatomic, copy) NSString* networkName;
@property(nonatomic, copy, nullable) NSString* fingerprint;
@property(nonatomic, assign) BOOL hasNetworkKey;

- (instancetype)initWithNetworkName:(NSString*)networkName
                         fingerprint:(nullable NSString*)fingerprint
                       hasNetworkKey:(BOOL)hasNetworkKey NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

NS_SWIFT_NAME(NetworkKeyBridge)
@interface CLPNetworkKeyBridge : NSObject

+ (nullable CLPNetworkKeyStatus*)loadStatusWithError:(NSError**)error NS_SWIFT_NAME(loadStatus());
+ (nullable CLPNetworkKeyStatus*)updateNetworkName:(NSString*)networkName
                                             error:(NSError**)error NS_SWIFT_NAME(updateNetworkName(_:));
+ (nullable CLPNetworkKeyStatus*)deriveAndStoreKeyWithNetworkName:(NSString*)networkName
                                                           secret:(NSString*)secret
                                                            error:(NSError**)error NS_SWIFT_NAME(deriveAndStoreKey(networkName:secret:));
+ (BOOL)clearNetworkKeyWithError:(NSError**)error NS_SWIFT_NAME(clearNetworkKey());

@end

NS_SWIFT_NAME(NetworkRuntimeBridge)
@interface CLPNetworkRuntimeBridge : NSObject

+ (BOOL)startWithError:(NSError**)error NS_SWIFT_NAME(start());
+ (void)stop;
+ (void)notifyNetworkKeyChanged;
+ (BOOL)isRunning;
+ (BOOL)hasPeerConnections;

@end

NS_ASSUME_NONNULL_END
