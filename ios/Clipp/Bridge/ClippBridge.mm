#import "ClippBridge.h"

#import <UIKit/UIKit.h>

#include "../../../src/ClipboardData.h"
#include "../../../src/KeyManager.h"
#include "../../../src/Logger.h"
#include "../../../src/MDNSThread.h"
#include "../../../src/NetworkRuntime.h"
#include "../../../src/PeerDisplay.h"
#include "../../../src/PeerManager.h"
#include "../../../src/Settings.h"
#include "../../../src/platform/uiClippPage.h"

#include <sodium.h>
#include <signal.h>

#include <cstring>
#include <mutex>
#include <string>

extern Settings g_settings;
extern KeyManager g_keyManager;
extern PeerDisplay g_peerDisplay;
extern PeerManager g_peerManager;
extern NetworkRuntime g_networkRuntime;

namespace {
constexpr NSInteger kClippNetworkKeyErrorBase = 4100;
constexpr NSInteger kClippNetworkRuntimeErrorBase = 4200;
constexpr NSInteger kClippIncomingClipboardErrorBase = 4300;
NSString* const kIncomingClipboardDidChangeNotification = @"net.clipp.ios.incoming-clipboard-did-change";
std::mutex g_runtimeBridgeMutex;
bool g_runtimeBridgeStarted = false;
std::mutex g_incomingClipboardMutex;
__strong CLPIncomingClipboardItem* g_latestIncomingClipboardItem = nil;

NSError* MakeError(NSInteger code, NSString* message) {
    return [NSError errorWithDomain:@"net.clipp.ios.network-key"
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
        AssignError(error, kClippNetworkKeyErrorBase + 1, @"libsodium failed to initialize.");
    }
    return initialized;
}

bool EnsureHostID(NSError** error) {
    HostId hostID;
    if (g_settings.ensureHostID(hostID)) {
        return true;
    }

    AssignError(error, kClippNetworkRuntimeErrorBase + 1, @"Unable to initialize host ID.");
    return false;
}

void LogNetworkStartupState() {
    g_logger.SetMinimumLevel(Logger::Level::DDebug);
    g_logger.log("iOS", Logger::Level::Info, "============================================================");
    g_logger.log("iOS", Logger::Level::Info, "Starting Clipp networking core.");
    g_logger.log("iOS", Logger::Level::Info, "Network name: %s", g_settings.networkName().c_str());

    std::string keyError;
    const std::wstring fingerprint = g_keyManager.GetNetworkFingerprintHash(nullptr, &keyError);
    if (!fingerprint.empty()) {
        g_logger.log("iOS", Logger::Level::Info, L"Network fingerprint: %ls", fingerprint.c_str());
    } else {
        g_logger.log("iOS", Logger::Level::Warning, "No network key configured yet: %s", keyError.c_str());
    }
}

bool StartNetworkRuntimeIfNeeded(NSError** error) {
    if (!EnsureSodium(error) || !EnsureHostID(error)) {
        return false;
    }

    signal(SIGPIPE, SIG_IGN);

    std::lock_guard<std::mutex> lock(g_runtimeBridgeMutex);
    if (g_runtimeBridgeStarted) {
        return true;
    }

    LogNetworkStartupState();
    if (!g_networkRuntime.Start()) {
        AssignError(error, kClippNetworkRuntimeErrorBase + 2, @"Unable to start network runtime.");
        return false;
    }

    g_runtimeBridgeStarted = true;
    g_logger.log("iOS", Logger::Level::Info, "Network runtime start requested.");
    return true;
}

std::string ToStdString(NSString* value) {
    if (value == nil) {
        return {};
    }

    const char* utf8 = value.UTF8String;
    return utf8 != nullptr ? std::string(utf8) : std::string{};
}

NSString* ToNSString(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

NSString* ToNSString(const std::wstring& value) {
    NSMutableString* text = [[NSMutableString alloc] initWithCapacity:value.size()];
    for (wchar_t ch : value) {
        [text appendFormat:@"%C", static_cast<unichar>(ch)];
    }
    return text;
}

bool IsPngStream(const std::vector<unsigned char>& data) {
    static constexpr unsigned char signature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    return data.size() >= sizeof(signature) && std::memcmp(data.data(), signature, sizeof(signature)) == 0;
}

NSString* ClipboardTextFromPayload(const ClipboardPayload& payload) {
    size_t textLength = payload.rawData.size();
    if (textLength > 0 && payload.rawData.back() == '\0') {
        --textLength;
    }
    if (textLength == 0) {
        return @"";
    }

    return [[NSString alloc] initWithBytes:payload.rawData.data()
                                    length:textLength
                                  encoding:NSUTF8StringEncoding];
}

void PublishIncomingClipboardItem(CLPIncomingClipboardItem* item) {
    {
        std::lock_guard<std::mutex> lock(g_incomingClipboardMutex);
        g_latestIncomingClipboardItem = item;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:kIncomingClipboardDidChangeNotification
                                                            object:nil];
    });
}

CLPNetworkKeyStatus* LoadNetworkKeyStatus(NSError** error) {
    if (!EnsureSodium(error)) {
        return nil;
    }

    NSString* networkName = ToNSString(g_settings.networkName());
    std::string keyError;
    const bool hasKey = g_keyManager.HaveNetworkKey();
    NSString* fingerprint = nil;

    if (hasKey) {
        std::wstring fingerprintText = g_keyManager.GetNetworkFingerprintHash(nullptr, &keyError);
        if (fingerprintText.empty()) {
            AssignError(error,
                        kClippNetworkKeyErrorBase + 2,
                        ToNSString("Unable to read network key fingerprint: " + keyError));
            return nil;
        }
        fingerprint = ToNSString(fingerprintText);
    }

    return [[CLPNetworkKeyStatus alloc] initWithNetworkName:networkName
                                                fingerprint:fingerprint
                                             hasNetworkKey:hasKey];
}
}

void CLPIOSReceiveClipboardPayload(const std::wstring& hostName, const ClipboardPayload& payload) {
    @autoreleasepool {
        NSString* deviceName = ToNSString(hostName);
        if (deviceName.length == 0) {
            deviceName = @"Unknown device";
        }

        CLPIncomingClipboardItem* item = nil;
        if (payload.formatId == CF_UNICODETEXT) {
            NSString* text = ClipboardTextFromPayload(payload);
            if (text.length == 0 && payload.rawData.size() != 0) {
                g_logger.log("iOS", Logger::Level::Warning, L"Incoming text clipboard payload could not be decoded as UTF-8.");
                return;
            }

            item = [[CLPIncomingClipboardItem alloc] initWithIdentifier:NSUUID.UUID.UUIDString
                                                             deviceName:deviceName
                                                             receivedAt:NSDate.date
                                                                   kind:CLPIncomingClipboardPayloadKindText
                                                                   text:text ?: @""
                                                           imagePNGData:nil];
        } else if (payload.formatId == CF_DIB) {
            if (!IsPngStream(payload.rawData)) {
                g_logger.log("iOS", Logger::Level::Warning, L"Incoming image clipboard payload was not a PNG stream (%zu bytes).", payload.rawData.size());
                return;
            }

            NSData* imageData = [NSData dataWithBytes:payload.rawData.data()
                                               length:payload.rawData.size()];
            item = [[CLPIncomingClipboardItem alloc] initWithIdentifier:NSUUID.UUID.UUIDString
                                                             deviceName:deviceName
                                                             receivedAt:NSDate.date
                                                                   kind:CLPIncomingClipboardPayloadKindImage
                                                                   text:nil
                                                           imagePNGData:imageData];
        } else {
            g_logger.log("iOS", Logger::Level::Warning, L"Unsupported incoming clipboard format ID %u; payload ignored.", payload.formatId);
            return;
        }

        PublishIncomingClipboardItem(item);
    }
}

@implementation CLPIncomingClipboardItem

- (instancetype)initWithIdentifier:(NSString*)identifier
                        deviceName:(NSString*)deviceName
                        receivedAt:(NSDate*)receivedAt
                              kind:(CLPIncomingClipboardPayloadKind)kind
                              text:(NSString*)text
                      imagePNGData:(NSData*)imagePNGData {
    self = [super init];
    if (self) {
        _identifier = [identifier copy];
        _deviceName = [deviceName copy];
        _receivedAt = [receivedAt copy];
        _kind = kind;
        _text = [text copy];
        _imagePNGData = [imagePNGData copy];
    }
    return self;
}

- (BOOL)hasTextPayload {
    return self.kind == CLPIncomingClipboardPayloadKindText && self.text.length > 0;
}

- (BOOL)hasImagePayload {
    return self.kind == CLPIncomingClipboardPayloadKindImage && self.imagePNGData.length > 0;
}

@end

@implementation CLPIncomingClipboardBridge

+ (NSString*)didChangeNotificationName {
    return kIncomingClipboardDidChangeNotification;
}

+ (CLPIncomingClipboardItem*)latestItem {
    std::lock_guard<std::mutex> lock(g_incomingClipboardMutex);
    __strong CLPIncomingClipboardItem* item = g_latestIncomingClipboardItem;
    return item;
}

+ (BOOL)copyItem:(CLPIncomingClipboardItem*)item
           error:(NSError**)error {
    if (item == nil) {
        AssignError(error, kClippIncomingClipboardErrorBase + 1, @"No clipboard item is available to copy.");
        return NO;
    }

    if (item.hasTextPayload) {
        UIPasteboard.generalPasteboard.string = item.text;
        return YES;
    }

    if (item.hasImagePayload) {
        UIImage* image = [UIImage imageWithData:item.imagePNGData];
        if (image == nil) {
            AssignError(error, kClippIncomingClipboardErrorBase + 2, @"Unable to decode clipboard image.");
            return NO;
        }

        UIPasteboard.generalPasteboard.image = image;
        return YES;
    }

    AssignError(error, kClippIncomingClipboardErrorBase + 3, @"Unsupported clipboard item.");
    return NO;
}

@end

@implementation CLPNetworkKeyStatus

- (instancetype)initWithNetworkName:(NSString*)networkName
                         fingerprint:(NSString*)fingerprint
                       hasNetworkKey:(BOOL)hasNetworkKey {
    self = [super init];
    if (self) {
        _networkName = [networkName copy];
        _fingerprint = [fingerprint copy];
        _hasNetworkKey = hasNetworkKey;
    }
    return self;
}

@end

@implementation CLPNetworkKeyBridge

+ (CLPNetworkKeyStatus*)loadStatusWithError:(NSError**)error {
    return LoadNetworkKeyStatus(error);
}

+ (CLPNetworkKeyStatus*)updateNetworkName:(NSString*)networkName
                                    error:(NSError**)error {
    if (!EnsureSodium(error)) {
        return nil;
    }

    const std::string newNetworkName = ToStdString(networkName);
    if (newNetworkName.empty()) {
        AssignError(error, kClippNetworkKeyErrorBase + 3, @"Network name cannot be empty.");
        return nil;
    }

    const std::string currentNetworkName = g_settings.networkName();
    if (newNetworkName != currentNetworkName) {
        if (!g_settings.set_networkName(newNetworkName)) {
            AssignError(error, kClippNetworkKeyErrorBase + 5, @"Unable to store network name.");
            return nil;
        }

        g_logger.log("iOS", Logger::Level::Info, "Network name changed; clearing network key.");
        g_keyManager.ClearNetworkKey();
        MDNSNotifyNetworkKeyChange();
        g_peerManager.ClearPeers();
    }

    return LoadNetworkKeyStatus(error);
}

+ (CLPNetworkKeyStatus*)deriveAndStoreKeyWithNetworkName:(NSString*)networkName
                                                  secret:(NSString*)secret
                                                   error:(NSError**)error {
    if (!EnsureSodium(error)) {
        return nil;
    }

    std::string networkNameUtf8 = ToStdString(networkName);
    std::string secretUtf8 = ToStdString(secret);

    if (networkNameUtf8.empty()) {
        AssignError(error, kClippNetworkKeyErrorBase + 3, @"Network name cannot be empty.");
        return nil;
    }

    if (secretUtf8.size() < 8) {
        AssignError(error, kClippNetworkKeyErrorBase + 4, CLP_NS(CLP_UI_SECRET_TOO_SHORT));
        sodium_memzero(secretUtf8.data(), secretUtf8.capacity());
        return nil;
    }

    if (!g_settings.set_networkName(networkNameUtf8)) {
        AssignError(error, kClippNetworkKeyErrorBase + 5, @"Unable to store network name.");
        sodium_memzero(secretUtf8.data(), secretUtf8.capacity());
        return nil;
    }

    std::string keyInput = uiClippPage::BuildKeyDerivationInput(networkNameUtf8, secretUtf8);
    sodium_memzero(secretUtf8.data(), secretUtf8.capacity());

    KeyManager::NetworkKey networkKey{};
    const bool derived = g_keyManager.DeriveNetworkKey(keyInput, networkKey);
    sodium_memzero(keyInput.data(), keyInput.capacity());

    if (!derived) {
        AssignError(error, kClippNetworkKeyErrorBase + 6, @"Unable to derive network key.");
        return nil;
    }

    std::string storageError;
    const bool stored = g_keyManager.SetNetworkKey(networkKey, &storageError);
    sodium_memzero(networkKey.data(), networkKey.size());

    if (!stored) {
        AssignError(error,
                    kClippNetworkKeyErrorBase + 7,
                    ToNSString("Unable to store network key: " + storageError));
        return nil;
    }

    MDNSNotifyNetworkKeyChange();
    g_peerManager.ClearPeers();
    StartNetworkRuntimeIfNeeded(nullptr);

    return LoadNetworkKeyStatus(error);
}

+ (BOOL)clearNetworkKeyWithError:(NSError**)error {
    if (!EnsureSodium(error)) {
        return NO;
    }

    g_keyManager.ClearNetworkKey();
    MDNSNotifyNetworkKeyChange();
    g_peerManager.ClearPeers();
    return YES;
}

@end

@implementation CLPNetworkRuntimeBridge

+ (BOOL)startWithError:(NSError**)error {
    return StartNetworkRuntimeIfNeeded(error);
}

+ (void)stop {
    std::lock_guard<std::mutex> lock(g_runtimeBridgeMutex);
    if (!g_runtimeBridgeStarted) {
        return;
    }

    g_networkRuntime.Stop();
    g_peerManager.ClearPeers();
    g_runtimeBridgeStarted = false;
    g_logger.log("iOS", Logger::Level::Info, "Network runtime stopped.");
}

+ (void)notifyNetworkKeyChanged {
    MDNSNotifyNetworkKeyChange();
    g_peerManager.ClearPeers();
    StartNetworkRuntimeIfNeeded(nullptr);
}

+ (BOOL)isRunning {
    std::lock_guard<std::mutex> lock(g_runtimeBridgeMutex);
    return g_runtimeBridgeStarted;
}

+ (BOOL)hasPeerConnections {
    const auto peers = g_peerDisplay.Query();
    for (const auto& peer : peers) {
        if (peer.hasIncomingConnection || peer.hasOutgoingConnection) {
            return YES;
        }
    }
    return NO;
}

@end
