#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

NS_SWIFT_NAME(SharePayloadKind)
typedef NS_ENUM(NSInteger, CLPSharePayloadKind) {
    CLPSharePayloadKindText = 1,
    CLPSharePayloadKindPNG = 2,
    CLPSharePayloadKindJPEG = 3,
};

NS_SWIFT_NAME(SharePayload)
@interface CLPSharePayload : NSObject

@property(nonatomic, assign, readonly) CLPSharePayloadKind kind;
@property(nonatomic, copy, nullable, readonly) NSString* text;
@property(nonatomic, copy, nullable, readonly) NSData* pngData;
@property(nonatomic, copy, nullable, readonly) NSData* jpegData;

+ (instancetype)textPayloadWithText:(NSString*)text NS_SWIFT_NAME(text(_:));
+ (instancetype)pngPayloadWithData:(NSData*)pngData NS_SWIFT_NAME(pngData(_:));
+ (instancetype)jpegPayloadWithData:(NSData*)jpegData NS_SWIFT_NAME(jpegData(_:));
- (instancetype)init NS_UNAVAILABLE;

@end

NS_SWIFT_NAME(ShareSendResult)
@interface CLPShareSendResult : NSObject

@property(nonatomic, assign, readonly) NSInteger sentItemCount;
@property(nonatomic, assign, readonly) NSInteger reachedDeviceCount;

- (instancetype)initWithSentItemCount:(NSInteger)sentItemCount
                   reachedDeviceCount:(NSInteger)reachedDeviceCount NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

NS_SWIFT_NAME(ShareSenderBridge)
@interface CLPShareSenderBridge : NSObject

+ (nullable CLPShareSendResult*)sendPayloads:(NSArray<CLPSharePayload*>*)payloads
                                       error:(NSError**)error NS_SWIFT_NAME(send(_:));

@end

NS_ASSUME_NONNULL_END
