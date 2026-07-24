#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// The one-deep undo slot's current contents (mirrors the desktop
// ClipboardActions UndoSlotKind). The register UI captures
// pendingUndoRegisterName BEFORE calling undoDelete so it can re-select the
// resurrected row.
NS_SWIFT_NAME(RegisterUndoKind)
typedef NS_ENUM(NSInteger, CLPRegisterUndoKind) {
    CLPRegisterUndoKindNone = 0,
    CLPRegisterUndoKindRegister = 1,
    CLPRegisterUndoKindActivity = 2,
};

// One named register, materialized for the UI. The bridge reads g_registerStore
// directly (in-process), so it holds the real value even for private records —
// the UI is responsible for masking (PrivateLineView) and gating peek, exactly
// like the desktop popup.
NS_SWIFT_NAME(RegisterItem)
@interface CLPRegisterItem : NSObject

@property(nonatomic, copy, readonly) NSString* name;
// Capped single-window preview for list rows (never carries binary bytes).
@property(nonatomic, copy, readonly) NSString* previewText;
// Full text value; nil for a binary (image) register.
@property(nonatomic, copy, nullable, readonly) NSString* fullText;
@property(nonatomic, assign, readonly) BOOL isPrivate;
@property(nonatomic, assign, readonly) BOOL isBinary;
@property(nonatomic, assign, readonly) unsigned int imageFormatID;
// Raw image stream (header stripped) for a binary register; nil otherwise.
@property(nonatomic, copy, nullable, readonly) NSData* imageData;
@property(nonatomic, assign, readonly) unsigned long long valueSize;
// `touched` HLC wall time, for the row's relative-age label.
@property(nonatomic, copy, readonly) NSDate* touched;

- (instancetype)init NS_UNAVAILABLE;

@end

NS_SWIFT_NAME(RegisterBridge)
@interface CLPRegisterBridge : NSObject

+ (NSString*)didChangeNotificationName;

// Live registers, name-sorted, excluding tombstones and the reserved ""
// clipboard mirror. Arms the change watcher on first call.
+ (NSArray<CLPRegisterItem*>*)registers NS_SWIFT_NAME(registers());

// Touching read (refreshes `touched`, like `clipp paste`). nil if absent /
// tombstoned / expired. The UI uses the listed item for mere display and only
// calls this when it deliberately wants the LRU touch.
+ (nullable CLPRegisterItem*)readName:(NSString*)name NS_SWIFT_NAME(read(name:));

// Well-formedness check for the rename/save editors (NFC is the caller's job).
+ (BOOL)isValidName:(NSString*)name NS_SWIFT_NAME(isValidName(_:));

// "Copy": make this register the live clipboard here (local UIPasteboard) and
// across the mesh (a fresh clipboard event), implicitly moving it to the top of
// the activity stream. Disarms undo.
+ (BOOL)makeCurrent:(NSString*)name
              error:(NSError**)error NS_SWIFT_NAME(makeCurrent(name:));

// "Save": promote a clipboard activity item to a named register (text or image),
// broadcasting the write. Disarms undo.
+ (BOOL)saveActivityItemID:(unsigned long long)activityItemID
                    asName:(NSString*)name
               markPrivate:(BOOL)markPrivate
                     error:(NSError**)error NS_SWIFT_NAME(save(activityItemID:asName:markPrivate:));

// Tombstone a register everywhere and arm the one-deep undo slot.
+ (BOOL)deleteName:(NSString*)name
             error:(NSError**)error NS_SWIFT_NAME(delete(name:));

// LWW rename (upsert-new-then-tombstone-old, both broadcast). Disarms undo.
+ (BOOL)renameFrom:(NSString*)oldName
                to:(NSString*)newName
             error:(NSError**)error NS_SWIFT_NAME(rename(from:to:));

// Flip the PRIVATE flag in place (LWW overwrite, no tombstone). Disarms undo.
+ (BOOL)setName:(NSString*)name
        private:(BOOL)isPrivate
          error:(NSError**)error NS_SWIFT_NAME(setPrivate(name:private:));

// The single undo slot's state (see CLPRegisterUndoKind).
+ (CLPRegisterUndoKind)pendingUndoKind NS_SWIFT_NAME(pendingUndoKind());
// The register name a register-kind undo would resurrect ("" otherwise).
+ (nullable NSString*)pendingUndoRegisterName NS_SWIFT_NAME(pendingUndoRegisterName());
// Perform the pending undo (register: re-stamped upsert; activity: history-lane
// re-insert). NO if nothing is armed or the restore was refused.
+ (BOOL)undoDelete:(NSError**)error NS_SWIFT_NAME(undoDelete());

@end

NS_ASSUME_NONNULL_END
