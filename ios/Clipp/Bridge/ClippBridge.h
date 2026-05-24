#import <Foundation/Foundation.h>

#include "../../../src/platform/uistrings.h"

NS_ASSUME_NONNULL_BEGIN

NS_SWIFT_NAME(ClipboardPayloadKind)
typedef NS_ENUM(NSInteger, CLPClipboardPayloadKind) {
    CLPClipboardPayloadKindUnsupported = 0,
    CLPClipboardPayloadKindText = 1,
    CLPClipboardPayloadKindPrivateText = 2,
    CLPClipboardPayloadKindLink = 3,
    CLPClipboardPayloadKindImage = 4,
};

NS_SWIFT_NAME(ClipboardDirection)
typedef NS_ENUM(NSInteger, CLPClipboardDirection) {
    CLPClipboardDirectionIncoming = 1,
    CLPClipboardDirectionOutgoing = 2,
};

NS_SWIFT_NAME(ClipboardActivityItem)
@interface CLPClipboardActivityItem : NSObject

@property(nonatomic, copy, readonly) NSString* identifier;
@property(nonatomic, assign, readonly) unsigned long long activityItemID;
@property(nonatomic, copy, readonly) NSString* deviceName;
@property(nonatomic, copy, readonly) NSDate* timestamp;
@property(nonatomic, assign, readonly) CLPClipboardDirection direction;
@property(nonatomic, assign, readonly) CLPClipboardPayloadKind kind;
@property(nonatomic, copy, readonly) NSString* previewText;
@property(nonatomic, copy, readonly) NSString* detailText;
@property(nonatomic, copy, readonly) NSString* linkHost;
@property(nonatomic, copy, nullable, readonly) NSString* text;
@property(nonatomic, copy, nullable, readonly) NSData* imagePNGData;
@property(nonatomic, assign, readonly) BOOL hasTextPayload;
@property(nonatomic, assign, readonly) BOOL hasImagePayload;
@property(nonatomic, assign, readonly) BOOL isIncoming;
@property(nonatomic, assign, readonly) BOOL isOutgoing;

- (instancetype)initWithActivityItemID:(unsigned long long)activityItemID
                            identifier:(NSString*)identifier
                            deviceName:(NSString*)deviceName
                             timestamp:(NSDate*)timestamp
                             direction:(CLPClipboardDirection)direction
                                  kind:(CLPClipboardPayloadKind)kind
                           previewText:(NSString*)previewText
                            detailText:(NSString*)detailText
                              linkHost:(NSString*)linkHost
                                  text:(nullable NSString*)text
                          imagePNGData:(nullable NSData*)imagePNGData NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

NS_SWIFT_NAME(ClipboardActivityBridge)
@interface CLPClipboardActivityBridge : NSObject

+ (NSString*)didChangeNotificationName;
+ (NSArray<CLPClipboardActivityItem*>*)recentItems NS_SWIFT_NAME(recentItems());
+ (BOOL)copyItem:(CLPClipboardActivityItem*)item
           error:(NSError**)error NS_SWIFT_NAME(copy(_:));

@end

NS_SWIFT_NAME(OutgoingClipboardBridge)
@interface CLPOutgoingClipboardBridge : NSObject

+ (nullable CLPClipboardActivityItem*)sendCurrentPasteboardWithError:(NSError**)error NS_SWIFT_NAME(sendCurrentPasteboard());

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

NS_SWIFT_NAME(NetworkTrafficSnapshot)
@interface CLPNetworkTrafficSnapshot : NSObject

@property(nonatomic, assign, readonly) unsigned long long bytesSent;
@property(nonatomic, assign, readonly) unsigned long long bytesReceived;

- (instancetype)initWithBytesSent:(unsigned long long)bytesSent
                    bytesReceived:(unsigned long long)bytesReceived NS_DESIGNATED_INITIALIZER;
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
+ (CLPNetworkTrafficSnapshot*)trafficSnapshot;

@end

NS_ASSUME_NONNULL_END
