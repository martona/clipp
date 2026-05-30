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
    CLPClipboardPayloadKindPrivatePlaceholder = 5,
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
@property(nonatomic, assign, readonly) unsigned int imageFormatID;
@property(nonatomic, copy, nullable, readonly) NSData* imageData;
@property(nonatomic, assign, readonly) BOOL hasTextPayload;
@property(nonatomic, assign, readonly) BOOL hasImagePayload;
@property(nonatomic, assign, readonly) BOOL isIncoming;
@property(nonatomic, assign, readonly) BOOL isOutgoing;
// True iff the kind classification came from an explicit OS-level privacy
// marker on the source clipboard (as opposed to the receive-side single-line-
// no-whitespace heuristic). The UI uses this to attach a "private" badge on
// PrivateText entries and to distinguish PrivatePlaceholder rows.
@property(nonatomic, assign, readonly) BOOL sourceMarked;

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
                         imageFormatID:(unsigned int)imageFormatID
                              imageData:(nullable NSData*)imageData
                          sourceMarked:(BOOL)sourceMarked NS_DESIGNATED_INITIALIZER;
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

NS_SWIFT_NAME(NetworkPeerItem)
@interface CLPNetworkPeerItem : NSObject

@property(nonatomic, copy, readonly) NSString* identifier;
@property(nonatomic, copy, readonly) NSString* deviceName;
@property(nonatomic, assign, readonly) BOOL hasIncomingConnection;
@property(nonatomic, assign, readonly) BOOL hasOutgoingConnection;

- (instancetype)initWithIdentifier:(NSString*)identifier
                         deviceName:(NSString*)deviceName
              hasIncomingConnection:(BOOL)hasIncomingConnection
              hasOutgoingConnection:(BOOL)hasOutgoingConnection NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

NS_SWIFT_NAME(NetworkPeerBridge)
@interface CLPNetworkPeerBridge : NSObject

+ (NSString*)didChangeNotificationName;
+ (NSArray<CLPNetworkPeerItem*>*)peers NS_SWIFT_NAME(peers());

@end

NS_SWIFT_NAME(DiagnosticLogRunColor)
typedef NS_ENUM(NSInteger, CLPDiagnosticLogRunColor) {
    CLPDiagnosticLogRunColorDefault = 0,
    CLPDiagnosticLogRunColorGray = 1,
    CLPDiagnosticLogRunColorDimCyan = 2,
    CLPDiagnosticLogRunColorCyan = 3,
    CLPDiagnosticLogRunColorGreen = 4,
    CLPDiagnosticLogRunColorYellow = 5,
    CLPDiagnosticLogRunColorRed = 6,
};

NS_SWIFT_NAME(DiagnosticLogRun)
@interface CLPDiagnosticLogRun : NSObject

@property(nonatomic, copy, readonly) NSString* text;
@property(nonatomic, assign, readonly) CLPDiagnosticLogRunColor color;

- (instancetype)initWithText:(NSString*)text
                       color:(CLPDiagnosticLogRunColor)color NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

NS_SWIFT_NAME(DiagnosticLogLine)
@interface CLPDiagnosticLogLine : NSObject

@property(nonatomic, copy, readonly) NSArray<CLPDiagnosticLogRun*>* runs;

- (instancetype)initWithRuns:(NSArray<CLPDiagnosticLogRun*>*)runs NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

NS_SWIFT_NAME(DiagnosticLogsBridge)
@interface CLPDiagnosticLogsBridge : NSObject

+ (NSString*)didChangeNotificationName;
+ (NSArray<CLPDiagnosticLogLine*>*)snapshot NS_SWIFT_NAME(snapshot());
+ (NSString*)plainText NS_SWIFT_NAME(plainText());
+ (NSUInteger)lineCount NS_SWIFT_NAME(lineCount());

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

NS_SWIFT_NAME(SettingsSnapshot)
@interface CLPSettingsSnapshot : NSObject

@property(nonatomic, assign, readonly) unsigned long long clipboardHistoryMemoryLimitBytes;
@property(nonatomic, assign, readonly) unsigned long long clipboardHistoryMaxAgeSeconds;
@property(nonatomic, assign, readonly) unsigned long long clipboardHistoryMaxItems;
@property(nonatomic, assign, readonly) NSInteger tcpPort;
@property(nonatomic, copy, readonly) NSString* listenerIP;
@property(nonatomic, copy, readonly) NSString* hostID;
@property(nonatomic, assign, readonly) BOOL hasHostIDCollisionWarning;
@property(nonatomic, assign, readonly) BOOL honorExternalPrivacyMarkers;

- (instancetype)initWithClipboardHistoryMemoryLimitBytes:(unsigned long long)clipboardHistoryMemoryLimitBytes
                         clipboardHistoryMaxAgeSeconds:(unsigned long long)clipboardHistoryMaxAgeSeconds
                              clipboardHistoryMaxItems:(unsigned long long)clipboardHistoryMaxItems
                                               tcpPort:(NSInteger)tcpPort
                                            listenerIP:(NSString*)listenerIP
                                                hostID:(NSString*)hostID
                             hasHostIDCollisionWarning:(BOOL)hasHostIDCollisionWarning
                           honorExternalPrivacyMarkers:(BOOL)honorExternalPrivacyMarkers NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

NS_SWIFT_NAME(SettingsBridge)
@interface CLPSettingsBridge : NSObject

+ (nullable CLPSettingsSnapshot*)loadSnapshotWithError:(NSError**)error NS_SWIFT_NAME(loadSnapshot());
+ (nullable CLPSettingsSnapshot*)updateClipboardHistoryMemoryLimitBytes:(unsigned long long)memoryLimitBytes
                                                           maxAgeSeconds:(unsigned long long)maxAgeSeconds
                                                                maxItems:(unsigned long long)maxItems
                                                                   error:(NSError**)error NS_SWIFT_NAME(updateClipboardHistory(memoryLimitBytes:maxAgeSeconds:maxItems:));
+ (nullable CLPSettingsSnapshot*)updateNetworkTcpPort:(NSInteger)tcpPort
                                           listenerIP:(NSString*)listenerIP
                                                error:(NSError**)error NS_SWIFT_NAME(updateNetwork(tcpPort:listenerIP:));
+ (nullable CLPSettingsSnapshot*)resetHostIDWithError:(NSError**)error NS_SWIFT_NAME(resetHostID());
+ (nullable CLPSettingsSnapshot*)updateHonorExternalPrivacyMarkers:(BOOL)honor
                                                              error:(NSError**)error NS_SWIFT_NAME(updateHonorExternalPrivacyMarkers(_:));

@end

NS_ASSUME_NONNULL_END
