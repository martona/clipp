#import "ClippRegisterBridge.h"

#include "../../../src/ClipboardActivityStore.h"
#include "../../../src/ClipboardFormat.h"
#include "../../../src/ClipboardPayload.h"
#include "../../../src/CryptoChannel.h"
#include "../../../src/Hlc.h"
#include "../../../src/HostId.h"
#include "../../../src/LocalPeerName.h"
#include "../../../src/Logger.h"
#include "../../../src/NetworkDefs.h"
#include "../../../src/PeerManager.h"
#include "../../../src/RegisterStore.h"
#include "../../../src/RegisterWire.h"
#include "../../../src/Settings.h"

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern Settings g_settings;
extern PeerManager g_peerManager;
extern RegisterStore g_registerStore;
extern ClipboardActivityStore g_clipboardActivityStore;

// Defined in ClippBridge.mm (where the pasteboard helpers + dedup guard live):
// sets the local UIPasteboard from `payload`, arms the dedup guard, records it in
// the activity store, and broadcasts to the mesh. The iOS analog of the desktop
// ClipboardActions::ApplyAndBroadcastPayload. Returns the new activity item id.
uint64_t CLPIOSPublishAndBroadcast(std::shared_ptr<const ClipboardPayload> payload);

namespace {

constexpr NSInteger kClippRegisterErrorBase = 4600;
NSString* const kRegistersDidChangeNotification = @"net.clipp.ios.registers-did-change";

// Row-preview cap: enough for a two-line row, never the whole (up-to-64 MB) value.
constexpr size_t kPreviewMaxBytes = 2048;

std::string ToStd(NSString* value) {
    if (value == nil) return {};
    const char* utf8 = value.UTF8String;
    return utf8 != nullptr ? std::string(utf8) : std::string{};
}

NSString* ToNS(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

// A byte-capped, UTF-8-boundary-safe prefix of `value` as an NSString. Trims any
// dangling multibyte lead/continuation bytes at the cut so initWithBytes never
// fails on a split code point.
NSString* PreviewNS(const std::string& value) {
    size_t n = value.size() < kPreviewMaxBytes ? value.size() : kPreviewMaxBytes;
    // Back off the cut until it isn't inside a multibyte sequence: drop trailing
    // 0x80..0xBF continuation bytes, and a trailing lead byte with no continuation.
    while (n > 0 && (static_cast<unsigned char>(value[n - 1]) & 0xC0) == 0x80) --n;
    if (n > 0 && (static_cast<unsigned char>(value[n - 1]) & 0x80) != 0) --n;
    return [[NSString alloc] initWithBytes:value.data()
                                    length:n
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

NSDate* TouchedDate(const RegisterRecord& rec) {
    return [NSDate dateWithTimeIntervalSince1970:static_cast<double>(rec.touched.wallMs) / 1000.0];
}

NSError* MakeError(NSInteger code, NSString* message) {
    return [NSError errorWithDomain:@"net.clipp.ios.registers"
                               code:code
                           userInfo:@{ NSLocalizedDescriptionKey: message }];
}

void AssignError(NSError** error, NSInteger code, NSString* message) {
    if (error != nullptr) {
        *error = MakeError(code, message);
    }
}

void PostRegistersChanged() {
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:kRegistersDidChangeNotification
                                                            object:nil];
    });
}

// UI-refresh listener. The store's listener list is append-only and every
// listener fires: the persistence runtime arms its own dirty hook independently
// (RegisterPersistenceRuntime.cpp), so the two never displace each other.
void EnsureRegisterWatcher() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_registerStore.AddChangeListener([] { PostRegistersChanged(); });
    });
}

// The mesh half of a local register write — identical to the desktop
// ClipboardActions::BroadcastRegisterRecord (persist the HLC floor, then push the
// stored record, value or tombstone, to register-serving peers).
void BroadcastRegisterRecord(const std::string& name) {
    g_settings.noteRegisterHlcWallMs(g_registerStore.ClockHighWater().wallMs);
    if (const auto stored = g_registerStore.GetForBroadcast(name)) {
        const auto regw = RegisterWire::EncodeRecord(*stored, 0);
        g_peerManager.BroadcastRegisterFrame({ 'R', 'E', 'G', 'W' }, regw);
    }
}

HostId LocalHost() {
    HostId host{};
    g_settings.getHostID(host);
    return host;
}

// The one-deep undo slot (mirrors ClipboardActions' UndoSlot). Touched only by
// user-initiated bridge calls, all on the main thread — no locking, same as
// desktop.
enum class UndoKind { None, Register, Activity };
struct UndoSlot {
    UndoKind kind = UndoKind::None;
    std::string registerName;
    std::string registerValue;
    uint8_t registerValueFlags = 0;
    HostId registerOrigin{};
    std::shared_ptr<const ClipboardPayload> payload;
};
UndoSlot g_undoSlot;

void DisarmUndo() {
    g_undoSlot = UndoSlot{};
}

}  // namespace

// Called from the clipboard bridge when a clipboard-side mutation (a re-share)
// lands: the desktop honesty rule — ANY successful user-initiated mutation
// disarms the one-deep undo slot, or a lit Undo would advertise more than it
// does. Posts the change notification so the Registers tab's undo bar drops
// immediately (a clipboard event fires no register notification on its own).
void CLPIOSDisarmRegisterUndo() {
    DisarmUndo();
    PostRegistersChanged();
}

@interface CLPRegisterItem ()
- (instancetype)initInternalWithName:(NSString*)name
                         previewText:(NSString*)previewText
                            fullText:(nullable NSString*)fullText
                           isPrivate:(BOOL)isPrivate
                            isBinary:(BOOL)isBinary
                       imageFormatID:(unsigned int)imageFormatID
                           imageData:(nullable NSData*)imageData
                           valueSize:(unsigned long long)valueSize
                             touched:(NSDate*)touched;
@end

@implementation CLPRegisterItem

- (instancetype)initInternalWithName:(NSString*)name
                         previewText:(NSString*)previewText
                            fullText:(NSString*)fullText
                           isPrivate:(BOOL)isPrivate
                            isBinary:(BOOL)isBinary
                       imageFormatID:(unsigned int)imageFormatID
                           imageData:(NSData*)imageData
                           valueSize:(unsigned long long)valueSize
                             touched:(NSDate*)touched {
    self = [super init];
    if (self) {
        _name = [name copy];
        _previewText = [previewText copy];
        _fullText = [fullText copy];
        _isPrivate = isPrivate;
        _isBinary = isBinary;
        _imageFormatID = imageFormatID;
        _imageData = [imageData copy];
        _valueSize = valueSize;
        _touched = [touched copy];
    }
    return self;
}

@end

namespace {

// Materialize a live value record (caller excludes "" + tombstones) for the UI.
CLPRegisterItem* MakeRegisterItem(const RegisterRecord& rec) {
    const bool isBinary = rec.IsBinary();
    NSString* fullText = nil;
    NSString* preview = @"";
    NSData* imageData = nil;
    unsigned int imageFormatID = 0;

    if (isBinary) {
        RegisterWire::BinaryValueInfo info{};
        if (RegisterWire::TryParseBinaryValue(rec.value, info)) {
            imageFormatID = info.formatId;
            if (info.streamOffset <= rec.value.size()) {
                imageData = [[NSData alloc] initWithBytes:rec.value.data() + info.streamOffset
                                                   length:rec.value.size() - info.streamOffset];
            }
        }
    } else {
        // Text (possibly private): the bridge holds the real value; the UI masks.
        fullText = ToNS(rec.value);
        preview = PreviewNS(rec.value);
    }

    return [[CLPRegisterItem alloc] initInternalWithName:ToNS(rec.name)
                                            previewText:preview
                                               fullText:fullText
                                              isPrivate:rec.IsPrivate() ? YES : NO
                                               isBinary:isBinary ? YES : NO
                                          imageFormatID:imageFormatID
                                              imageData:imageData
                                              valueSize:static_cast<unsigned long long>(rec.value.size())
                                                touched:TouchedDate(rec)];
}

}  // namespace

@implementation CLPRegisterBridge

+ (NSString*)didChangeNotificationName {
    return kRegistersDidChangeNotification;
}

+ (NSArray<CLPRegisterItem*>*)registers {
    EnsureRegisterWatcher();
    NSMutableArray<CLPRegisterItem*>* items = [NSMutableArray array];
    for (const RegisterRecord& rec : g_registerStore.List()) {
        if (rec.name.empty()) {
            continue;  // the "" clipboard mirror is not a user-addressable register
        }
        [items addObject:MakeRegisterItem(rec)];
    }
    return items;
}

+ (CLPRegisterItem*)readName:(NSString*)name {
    const auto rec = g_registerStore.Read(ToStd(name));  // touches (LRU), like `paste`
    if (!rec.has_value()) {
        return nil;
    }
    return MakeRegisterItem(*rec);
}

+ (BOOL)isValidName:(NSString*)name {
    return IsValidRegisterName(ToStd(name)) ? YES : NO;
}

+ (BOOL)makeCurrent:(NSString*)name error:(NSError**)error {
    const auto rec = g_registerStore.Read(ToStd(name));  // touch, like `paste`
    if (!rec.has_value()) {
        AssignError(error, kClippRegisterErrorBase + 1, @"That register no longer exists.");
        return NO;
    }

    // The same item `clipp copy` would send: canonical text plus the capture-
    // convention trailing NUL, or the raw stream with the header's format for a
    // binary register. Mirrors the desktop MakeRegisterCurrent build.
    ClipboardPayload payload;
    std::vector<unsigned char> bytes;
    if (rec->IsBinary()) {
        RegisterWire::BinaryValueInfo info{};
        if (!RegisterWire::TryParseBinaryValue(rec->value, info)) {
            AssignError(error, kClippRegisterErrorBase + 2, @"That register's image could not be read.");
            return NO;
        }
        payload.meta.formatId = info.formatId;
        bytes.assign(rec->value.begin() + static_cast<std::ptrdiff_t>(info.streamOffset), rec->value.end());
    } else {
        payload.meta.formatId = CLIPP_FORMAT_UTF8;
        bytes.assign(rec->value.begin(), rec->value.end());
        bytes.push_back('\0');
    }
    if (!payload.SetUncompressedBytes(std::move(bytes))) {
        AssignError(error, kClippRegisterErrorBase + 3, @"That register could not be copied.");
        return NO;
    }
    if (rec->IsPrivate()) {
        // Register privacy is content truth: ride the event as the same marker a
        // source app would set, so every activity list masks the preview.
        payload.meta.flags |= NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE;
    }

    // Fresh origin, fresh eventGuid: making a register current is a NEW clipboard
    // event originated here, not a re-share of a stored one.
    payload.StampOrigin(LocalHost(),
                        clipp::GetLocalPeerDisplayName("iPhone", CryptoChannel::HOSTNAME_MAX_BYTES).c_str(),
                        g_settings.nextOriginSequenceNumber());
    CLPIOSPublishAndBroadcast(std::make_shared<const ClipboardPayload>(std::move(payload)));
    DisarmUndo();
    return YES;
}

+ (BOOL)saveActivityItemID:(unsigned long long)activityItemID
                    asName:(NSString*)name
               markPrivate:(BOOL)markPrivate
                     error:(NSError**)error {
    const std::string nameStd = ToStd(name);
    if (!IsValidRegisterName(nameStd)) {
        AssignError(error, kClippRegisterErrorBase + 4, @"That register name isn't valid.");
        return NO;
    }
    const auto stored = g_clipboardActivityStore.PayloadReference(activityItemID);
    if (!stored) {
        AssignError(error, kClippRegisterErrorBase + 5, @"That clipboard item is no longer available.");
        return NO;
    }
    const std::vector<unsigned char>* bytes = stored->TryGetUncompressedBytes();
    if (bytes == nullptr || bytes->empty()) {
        AssignError(error, kClippRegisterErrorBase + 6, @"That item has no content to save.");
        return NO;
    }

    std::string value;
    uint8_t flags = markPrivate ? RegisterFlags::Private : uint8_t{ 0 };
    if (IsClippTextFormat(stored->meta.formatId)) {
        size_t n = bytes->size();
        if (n > 0 && bytes->back() == '\0') --n;  // logical content, drop transport NUL
        value.assign(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(n));
    } else if (IsClippImageFormat(stored->meta.formatId)) {
        value = RegisterWire::EncodeBinaryValue(stored->meta.formatId, bytes->data(), bytes->size());
        flags |= RegisterFlags::BinaryHeader;
    } else {
        AssignError(error, kClippRegisterErrorBase + 7, @"That item type can't be saved as a register.");
        return NO;
    }

    if (g_registerStore.UpsertWithFlags(nameStd, std::move(value), flags, LocalHost())
        != RegisterStore::WriteResult::Ok) {
        AssignError(error, kClippRegisterErrorBase + 8, @"That register couldn't be saved.");
        return NO;
    }
    BroadcastRegisterRecord(nameStd);
    DisarmUndo();
    return YES;
}

+ (BOOL)deleteName:(NSString*)name error:(NSError**)error {
    const std::string nameStd = ToStd(name);
    // Snapshot before the tombstone lands; this is what undo would bring back.
    const auto rec = g_registerStore.Read(nameStd);
    if (g_registerStore.Delete(nameStd) != RegisterStore::DeleteResult::Deleted) {
        AssignError(error, kClippRegisterErrorBase + 9, @"That register no longer exists.");
        return NO;
    }
    if (rec.has_value()) {
        g_undoSlot = UndoSlot{};
        g_undoSlot.kind = UndoKind::Register;
        g_undoSlot.registerName = rec->name;
        g_undoSlot.registerValue = rec->value;
        g_undoSlot.registerValueFlags = static_cast<uint8_t>(
            rec->flags & (RegisterFlags::Private | RegisterFlags::BinaryHeader));
        g_undoSlot.registerOrigin = rec->originHostId;
    }
    BroadcastRegisterRecord(nameStd);  // GetForBroadcast surfaces the fresh tombstone
    return YES;
}

+ (BOOL)renameFrom:(NSString*)oldName to:(NSString*)newName error:(NSError**)error {
    const std::string oldStd = ToStd(oldName);
    const std::string newStd = ToStd(newName);
    if (newStd == oldStd) {
        return YES;
    }
    if (!IsValidRegisterName(newStd)) {
        AssignError(error, kClippRegisterErrorBase + 10, @"That register name isn't valid.");
        return NO;
    }
    auto rec = g_registerStore.Read(oldStd);  // live values only; the touch is harmless
    if (!rec.has_value()) {
        AssignError(error, kClippRegisterErrorBase + 11, @"That register no longer exists.");
        return NO;
    }
    const uint8_t valueFlags = static_cast<uint8_t>(
        rec->flags & (RegisterFlags::Private | RegisterFlags::BinaryHeader));
    if (g_registerStore.UpsertWithFlags(newStd, std::move(rec->value), valueFlags, LocalHost())
        != RegisterStore::WriteResult::Ok) {
        AssignError(error, kClippRegisterErrorBase + 12, @"That register couldn't be renamed.");
        return NO;
    }
    BroadcastRegisterRecord(newStd);
    if (g_registerStore.Delete(oldStd) == RegisterStore::DeleteResult::Deleted) {
        BroadcastRegisterRecord(oldStd);
    }
    DisarmUndo();
    return YES;
}

+ (BOOL)setName:(NSString*)name private:(BOOL)isPrivate error:(NSError**)error {
    const std::string nameStd = ToStd(name);
    auto rec = g_registerStore.Read(nameStd);  // live values only
    if (!rec.has_value()) {
        AssignError(error, kClippRegisterErrorBase + 13, @"That register no longer exists.");
        return NO;
    }
    uint8_t flags = static_cast<uint8_t>(rec->flags & RegisterFlags::BinaryHeader);
    if (isPrivate) {
        flags |= RegisterFlags::Private;
    }
    if (g_registerStore.UpsertWithFlags(nameStd, std::move(rec->value), flags, LocalHost())
        != RegisterStore::WriteResult::Ok) {
        AssignError(error, kClippRegisterErrorBase + 14, @"That register couldn't be updated.");
        return NO;
    }
    BroadcastRegisterRecord(nameStd);
    DisarmUndo();
    return YES;
}

+ (CLPRegisterUndoKind)pendingUndoKind {
    switch (g_undoSlot.kind) {
    case UndoKind::Register:
        return CLPRegisterUndoKindRegister;
    case UndoKind::Activity:
        return CLPRegisterUndoKindActivity;
    case UndoKind::None:
    default:
        return CLPRegisterUndoKindNone;
    }
}

+ (NSString*)pendingUndoRegisterName {
    return g_undoSlot.kind == UndoKind::Register ? ToNS(g_undoSlot.registerName) : nil;
}

+ (BOOL)undoDelete:(NSError**)error {
    if (g_undoSlot.kind == UndoKind::Register) {
        // Re-stamped upsert: name, content, privacy, binariness and origin device
        // come back exactly; a fresh `written` clock outranks the delete's tombstone
        // everywhere. Value copied so a refused write leaves the slot retryable.
        if (g_registerStore.UpsertWithFlags(g_undoSlot.registerName,
                                            std::string(g_undoSlot.registerValue),
                                            g_undoSlot.registerValueFlags,
                                            g_undoSlot.registerOrigin)
            != RegisterStore::WriteResult::Ok) {
            AssignError(error, kClippRegisterErrorBase + 15, @"That register couldn't be restored.");
            return NO;
        }
        BroadcastRegisterRecord(g_undoSlot.registerName);
        DisarmUndo();
        return YES;
    }

    if (g_undoSlot.kind == UndoKind::Activity && g_undoSlot.payload) {
        // Local re-insert exactly as the item lived (original guid + timestamp),
        // then a SYNC_REPLAY-lane rebroadcast so no live clipboard is touched.
        auto restored = std::make_shared<ClipboardPayload>();
        restored->meta = g_undoSlot.payload->meta;
        restored->meta.flags &= ~(NetworkDefs::CLPM_FLAG_SYNC_REPLAY | NetworkDefs::CLPM_FLAG_RELAY);
        restored->SetEncodedBytes(std::vector<unsigned char>(g_undoSlot.payload->EncodedBytes()));
        g_clipboardActivityStore.Add(restored);

        auto wire = std::make_shared<ClipboardPayload>();
        wire->meta = restored->meta;
        wire->meta.flags |= NetworkDefs::CLPM_FLAG_SYNC_REPLAY;
        wire->SetEncodedBytes(std::vector<unsigned char>(restored->EncodedBytes()));
        g_peerManager.BroadcastClipboard(std::move(wire));

        DisarmUndo();
        return YES;
    }

    AssignError(error, kClippRegisterErrorBase + 16, @"There's nothing to undo.");
    return NO;
}

@end
