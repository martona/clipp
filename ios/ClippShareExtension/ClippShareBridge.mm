#import "ClippShareBridge.h"

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
        AssignError(error, kClippShareErrorBase + 3, @"No network key is configured. Open Clipp to finish setup.");
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

@end
