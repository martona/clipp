#import "ClippShareBridge.h"

#include "../../src/ClipboardPersistence.h"
#include "../../src/ClipboardWire.h"
#include "../../src/ClipboardPayload.h"
#include "../../src/CryptoChannel.h"
#include "../../src/KeyManager.h"
#include "../../src/LocalPeerName.h"
#include "../../src/Logger.h"
#include "../../src/MDNSDiscovery.h"
#include "../../src/MDNSProtocol.h"
#include "../../src/OneShotPeer.h"
#include "../../src/Settings.h"
#include "../../src/platform.h"
#include "../../src/utils_socket.h"

#include <sodium.h>

#include <signal.h>
#include <string>
#include <utility>
#include <vector>

extern Settings g_settings;
extern KeyManager g_keyManager;

namespace {
constexpr NSInteger kClippShareErrorBase = 5100;

NSError* MakeError(NSInteger code, NSString* message) {
    return [NSError errorWithDomain:@"net.clipp.ios.share"
                               code:code
                           userInfo:@{ NSLocalizedDescriptionKey: message }];
}

void AssignError(NSError** error, NSInteger code, NSString* message) {
    if (error != nullptr) {
        *error = MakeError(code, message);
    }
}

bool EnsureSodium(NSError** error) {
    static dispatch_once_t once;
    static bool initialized = false;
    dispatch_once(&once, ^{
        initialized = sodium_init() >= 0;
    });

    if (!initialized) {
        AssignError(error, kClippShareErrorBase + 1, @"libsodium failed to initialize.");
    }
    return initialized;
}

bool EnsureHostID(NSError** error) {
    HostId hostID;
    if (g_settings.ensureHostID(hostID)) {
        return true;
    }

    AssignError(error, kClippShareErrorBase + 2, @"Unable to initialize this device's network identity.");
    return false;
}

std::string ToStdString(NSString* value) {
    if (value == nil) {
        return {};
    }

    const char* utf8 = value.UTF8String;
    return utf8 != nullptr ? std::string(utf8) : std::string{};
}

bool PayloadFromSharePayload(CLPSharePayload* sharePayload, ClipboardPayload& payload) {
    payload = ClipboardPayload{};

    if (sharePayload.kind == CLPSharePayloadKindText) {
        const std::string text = ToStdString(sharePayload.text);
        if (text.empty()) {
            return false;
        }

        payload.meta.formatId = CLIPP_FORMAT_UTF8;
        std::vector<unsigned char> bytes(text.begin(), text.end());
        bytes.push_back('\0');
        return payload.SetUncompressedBytes(std::move(bytes));
    }

    if (sharePayload.kind == CLPSharePayloadKindJPEG) {
        NSData* jpegData = sharePayload.jpegData;
        if (jpegData.length == 0) {
            return false;
        }

        payload.meta.formatId = CLIPP_FORMAT_JPEG;
        std::vector<unsigned char> bytes;
        const auto* src = static_cast<const unsigned char*>(jpegData.bytes);
        bytes.assign(src, src + jpegData.length);
        return payload.SetUncompressedBytes(std::move(bytes));
    }

    if (sharePayload.kind == CLPSharePayloadKindPNG) {
        NSData* pngData = sharePayload.pngData;
        if (pngData.length == 0) {
            return false;
        }

        payload.meta.formatId = CLIPP_FORMAT_PNG;
        std::vector<unsigned char> bytes;
        const auto* src = static_cast<const unsigned char*>(pngData.bytes);
        bytes.assign(src, src + pngData.length);
        return payload.SetUncompressedBytes(std::move(bytes));
    }

    return false;
}

NSString* const kAppGroupIdentifier = @"group.net.clipp.ios";

// <app group>/ShareInbox/<fp8>/ — the extension's ONLY shared write. It never
// touches the main app's snapshot blobs (the desktop concurrency lesson): one
// guid-named sealed file per item, atomically renamed into place by the seal
// layer, ingested and deleted by the main app. Layout must stay in lockstep
// with ClippClipboardPersistenceCore.mm's InboxDirectoryPath.
bool InboxDirectoryPath(std::string& outDir) {
    @autoreleasepool {
        NSURL* container = [NSFileManager.defaultManager
            containerURLForSecurityApplicationGroupIdentifier:kAppGroupIdentifier];
        if (container == nil) {
            return false;
        }
        const std::wstring fingerprint = g_keyManager.GetNetworkFingerprintHash();
        std::string tag;
        for (size_t i = 0; i < fingerprint.size() && i < 8; ++i) {
            tag.push_back(static_cast<char>(fingerprint[i]));
        }
        if (tag.empty()) {
            return false;
        }
        NSString* dir = [[container.path stringByAppendingPathComponent:@"ShareInbox"]
            stringByAppendingPathComponent:[NSString stringWithUTF8String:tag.c_str()]];
        if (![NSFileManager.defaultManager createDirectoryAtPath:dir
                                     withIntermediateDirectories:YES
                                                      attributes:nil
                                                           error:nil]) {
            return false;
        }
        const char* utf8 = dir.UTF8String;
        if (utf8 == nullptr) {
            return false;
        }
        outDir = utf8;
        return true;
    }
}

std::string GuidHex(const uint8_t (&guid)[16]) {
    static const char* kHex = "0123456789abcdef";
    std::string hex;
    hex.reserve(32);
    for (size_t i = 0; i < 16; ++i) {
        hex.push_back(kHex[guid[i] >> 4]);
        hex.push_back(kHex[guid[i] & 0x0F]);
    }
    return hex;
}

}

@interface CLPSharePayload ()

- (instancetype)initWithKind:(CLPSharePayloadKind)kind
                        text:(nullable NSString*)text
                     pngData:(nullable NSData*)pngData
                    jpegData:(nullable NSData*)jpegData NS_DESIGNATED_INITIALIZER;

@end

@implementation CLPSharePayload

+ (instancetype)textPayloadWithText:(NSString*)text {
    CLPSharePayload* payload = [[CLPSharePayload alloc] initWithKind:CLPSharePayloadKindText
                                                                text:text
                                                             pngData:nil
                                                            jpegData:nil];
    return payload;
}

+ (instancetype)pngPayloadWithData:(NSData*)pngData {
    CLPSharePayload* payload = [[CLPSharePayload alloc] initWithKind:CLPSharePayloadKindPNG
                                                                text:nil
                                                             pngData:pngData
                                                            jpegData:nil];
    return payload;
}

+ (instancetype)jpegPayloadWithData:(NSData*)jpegData {
    CLPSharePayload* payload = [[CLPSharePayload alloc] initWithKind:CLPSharePayloadKindJPEG
                                                                text:nil
                                                             pngData:nil
                                                            jpegData:jpegData];
    return payload;
}

- (instancetype)initWithKind:(CLPSharePayloadKind)kind
                        text:(NSString*)text
                     pngData:(NSData*)pngData
                    jpegData:(NSData*)jpegData {
    self = [super init];
    if (self) {
        _kind = kind;
        _text = [text copy];
        _pngData = [pngData copy];
        _jpegData = [jpegData copy];
    }
    return self;
}

@end

@implementation CLPShareSendResult

- (instancetype)initWithSentItemCount:(NSInteger)sentItemCount
                 relayedViaDeviceName:(NSString*)relayedViaDeviceName {
    self = [super init];
    if (self) {
        _sentItemCount = sentItemCount;
        _relayedViaDeviceName = [relayedViaDeviceName copy];
    }
    return self;
}

@end

@implementation CLPShareSenderBridge

+ (CLPShareSendResult*)sendPayloads:(NSArray<CLPSharePayload*>*)payloads
                               error:(NSError**)error {
    if (payloads.count == 0) {
        AssignError(error, kClippShareErrorBase + 1, @"No supported shared items were found.");
        return nil;
    }

    if (!EnsureSodium(error) || !EnsureHostID(error)) {
        return nil;
    }

    signal(SIGPIPE, SIG_IGN);

    if (!g_keyManager.HaveNetworkKey()) {
        AssignError(error, kClippShareErrorBase + 3, @"Not paired yet. Open Clipp to finish setup.");
        return nil;
    }

    HostId localHostId;
    g_settings.getHostID(localHostId);
    const std::string localHostName = clipp::GetLocalPeerDisplayName("iPhone", CryptoChannel::HOSTNAME_MAX_BYTES);

    std::vector<ClipboardPayload> clipboardPayloads;
    clipboardPayloads.reserve(payloads.count);
    for (CLPSharePayload* payload in payloads) {
        ClipboardPayload clipboardPayload{};
        if (PayloadFromSharePayload(payload, clipboardPayload)) {
            clipboardPayload.StampOrigin(localHostId, localHostName.c_str(), g_settings.nextOriginSequenceNumber());
            clipboardPayloads.push_back(std::move(clipboardPayload));
        }
    }

    if (clipboardPayloads.empty()) {
        AssignError(error, kClippShareErrorBase + 5, @"No supported shared items could be prepared.");
        return nil;
    }

    const NSInteger itemCount = static_cast<NSInteger>(clipboardPayloads.size());

    // Relay through the first reachable peer; it rebroadcasts to the synced mesh.
    // includeSelf=false: the extension can't assume its own main app is running to relay.
    const auto via = OneShot::RelayPayloads(std::move(clipboardPayloads), localHostId, localHostName, /*includeSelf=*/false);
    if (!via) {
        AssignError(error, kClippShareErrorBase + 7, @"No trusted device was reachable to relay the shared items.");
        return nil;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Share extension relayed %ld item(s) via %hs.",
        static_cast<long>(itemCount), via->deviceName.c_str());
    NSString* viaName = [NSString stringWithUTF8String:via->deviceName.c_str()];
    return [[CLPShareSendResult alloc] initWithSentItemCount:itemCount
                                       relayedViaDeviceName:(viaName ?: @"a nearby device")];
}

+ (NSNumber*)stashPayloadsForLaterDelivery:(NSArray<CLPSharePayload*>*)payloads
                                     error:(NSError**)error {
    if (payloads.count == 0) {
        AssignError(error, kClippShareErrorBase + 1, @"No supported shared items were found.");
        return nil;
    }
    if (!EnsureSodium(error) || !EnsureHostID(error)) {
        return nil;
    }
    if (!g_keyManager.HaveNetworkKey()) {
        AssignError(error, kClippShareErrorBase + 3, @"Not paired yet. Open Clipp to finish setup.");
        return nil;
    }

    std::string inboxDir;
    if (!InboxDirectoryPath(inboxDir)) {
        AssignError(error, kClippShareErrorBase + 9, @"Couldn't reach Clipp's shared storage.");
        return nil;
    }

    KeyManager::NetworkKey subkey{};
    if (!g_keyManager.GetKey(KeyManager::KeyRole::ShareInbox, subkey)) {
        AssignError(error, kClippShareErrorBase + 10, @"Couldn't prepare the saved share.");
        return nil;
    }
    ClipboardPersistence::SealKey key{};
    std::copy(subkey.begin(), subkey.end(), key.begin());
    sodium_memzero(subkey.data(), subkey.size());

    HostId localHostId;
    g_settings.getHostID(localHostId);
    const std::string localHostName = clipp::GetLocalPeerDisplayName("iPhone", CryptoChannel::HOSTNAME_MAX_BYTES);

    // Origin-stamped at STASH time — the share was authored NOW, on this phone;
    // when the main app ingests the file hours later it keeps this identity and
    // timestamp (a deferred share lands in history at the moment it was made).
    NSInteger stashed = 0;
    for (CLPSharePayload* sharePayload in payloads) {
        ClipboardPayload payload{};
        if (!PayloadFromSharePayload(sharePayload, payload)) {
            continue;
        }
        payload.StampOrigin(localHostId, localHostName.c_str(), g_settings.nextOriginSequenceNumber());
        const std::string path = inboxDir + "/" + GuidHex(payload.meta.eventGuid) + ".bin";
        if (ClipboardPersistence::SaveInboxItemFile(path, payload, key)) {
            ++stashed;
        }
    }
    sodium_memzero(key.data(), key.size());

    if (stashed == 0) {
        AssignError(error, kClippShareErrorBase + 11, @"The shared items couldn't be saved.");
        return nil;
    }
    g_logger.log(__FUNCTION__, Logger::Level::Info,
                 L"Share extension stashed %ld item(s) for deferred delivery.",
                 static_cast<long>(stashed));
    return @(stashed);
}

@end
