#import <Foundation/Foundation.h>

#include "../../../src/platform/uistrings.h"

NS_ASSUME_NONNULL_BEGIN

NS_SWIFT_NAME(IncomingClipboardPayloadKind)
typedef NS_ENUM(NSInteger, CLPIncomingClipboardPayloadKind) {
    CLPIncomingClipboardPayloadKindText = 1,
    CLPIncomingClipboardPayloadKindImage = 2,
};

NS_SWIFT_NAME(IncomingClipboardItem)
@interface CLPIncomingClipboardItem : NSObject

@property(nonatomic, copy, readonly) NSString* identifier;
@property(nonatomic, copy, readonly) NSString* deviceName;
@property(nonatomic, copy, readonly) NSDate* receivedAt;
@property(nonatomic, assign, readonly) CLPIncomingClipboardPayloadKind kind;
@property(nonatomic, copy, nullable, readonly) NSString* text;
@property(nonatomic, copy, nullable, readonly) NSData* imagePNGData;
@property(nonatomic, assign, readonly) BOOL hasTextPayload;
@property(nonatomic, assign, readonly) BOOL hasImagePayload;

- (instancetype)initWithIdentifier:(NSString*)identifier
                        deviceName:(NSString*)deviceName
                        receivedAt:(NSDate*)receivedAt
                              kind:(CLPIncomingClipboardPayloadKind)kind
                              text:(nullable NSString*)text
                      imagePNGData:(nullable NSData*)imagePNGData NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

NS_SWIFT_NAME(IncomingClipboardBridge)
@interface CLPIncomingClipboardBridge : NSObject

+ (NSString*)didChangeNotificationName;
+ (nullable CLPIncomingClipboardItem*)latestItem;
+ (BOOL)copyItem:(CLPIncomingClipboardItem*)item
           error:(NSError**)error NS_SWIFT_NAME(copy(_:));

@end

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
