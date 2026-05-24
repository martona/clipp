#import "ClippBridge.h"

#import <UIKit/UIKit.h>

#include "../../../src/ClipboardActivityStore.h"
#include "../../../src/ClipboardData.h"
#include "../../../src/ClipboardHashGuard.h"
#include "../../../src/KeyManager.h"
#include "../../../src/Logger.h"
#include "../../../src/MDNSThread.h"
#include "../../../src/NetworkRuntime.h"
#include "../../../src/PeerDisplay.h"
#include "../../../src/PeerManager.h"
#include "../../../src/Settings.h"
#include "../../../src/platform/uiClippPage.h"
#include "../../../src/platform/uiSettingsPage.h"

#include <sodium.h>
#include <signal.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "../../../src/ClipboardActivityStore.cpp"

extern Settings g_settings;
extern KeyManager g_keyManager;
extern PeerDisplay g_peerDisplay;
extern PeerManager g_peerManager;
extern NetworkRuntime g_networkRuntime;

namespace {
constexpr NSInteger kClippNetworkKeyErrorBase = 4100;
constexpr NSInteger kClippNetworkRuntimeErrorBase = 4200;
constexpr NSInteger kClippClipboardActivityErrorBase = 4300;
constexpr NSInteger kClippOutgoingClipboardErrorBase = 4400;
constexpr NSInteger kClippSettingsErrorBase = 4500;
NSString* const kClipboardActivityDidChangeNotification = @"net.clipp.ios.clipboard-activity-did-change";
std::mutex g_runtimeBridgeMutex;
bool g_runtimeBridgeStarted = false;
std::once_flag g_clipboardHistoryLimitsOnce;
std::once_flag g_clipboardActivityWatcherOnce;
std::size_t g_clipboardActivityWatcherID = 0;
ClipboardActivityStore g_clipboardActivityStore;
ClipboardHashGuard g_clipboardHashGuard;

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

bool RestartNetworkRuntime(NSError** error) {
    if (!EnsureSodium(error) || !EnsureHostID(error)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_runtimeBridgeMutex);
    if (g_runtimeBridgeStarted) {
        if (!g_networkRuntime.Restart()) {
            AssignError(error, kClippSettingsErrorBase + 1, @"Unable to restart network runtime.");
            return false;
        }
    } else {
        if (!g_networkRuntime.Start()) {
            AssignError(error, kClippSettingsErrorBase + 2, @"Unable to start network runtime.");
            return false;
        }
        g_runtimeBridgeStarted = true;
    }

    g_logger.log("iOS", Logger::Level::Info, "Network runtime restarted after settings change.");
    return true;
}

void ApplyClipboardHistoryLimitsFromSettings() {
    g_clipboardActivityStore.SetLimits(
        g_settings.clipboardHistoryMemoryLimitBytes(),
        g_settings.clipboardHistoryMaxAgeSeconds(),
        g_settings.clipboardHistoryMaxItems());
}

void EnsureClipboardHistoryLimitsApplied() {
    std::call_once(g_clipboardHistoryLimitsOnce, [] {
        ApplyClipboardHistoryLimitsFromSettings();
    });
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

void PostClipboardActivityChanged() {
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:kClipboardActivityDidChangeNotification
                                                            object:nil];
    });
}

void ClipboardActivityWatcher(const ClipboardActivityUpdate&, void*) {
    PostClipboardActivityChanged();
}

void EnsureClipboardActivityWatcher() {
    EnsureClipboardHistoryLimitsApplied();
    std::call_once(g_clipboardActivityWatcherOnce, [] {
        const ClipboardActivityRegistration registration =
            g_clipboardActivityStore.QueryAndRegister(ClipboardActivityWatcher, nullptr);
        g_clipboardActivityWatcherID = registration.watcherID;
    });
}

CLPClipboardPayloadKind ToBridgePayloadKind(ClipboardActivityPayloadKind kind) {
    switch (kind) {
    case ClipboardActivityPayloadKind::Text:
        return CLPClipboardPayloadKindText;
    case ClipboardActivityPayloadKind::PrivateText:
        return CLPClipboardPayloadKindPrivateText;
    case ClipboardActivityPayloadKind::Link:
        return CLPClipboardPayloadKindLink;
    case ClipboardActivityPayloadKind::Image:
        return CLPClipboardPayloadKindImage;
    case ClipboardActivityPayloadKind::Unsupported:
    default:
        return CLPClipboardPayloadKindUnsupported;
    }
}

CLPClipboardDirection ToBridgeDirection(ClipboardActivityDirection direction) {
    return direction == ClipboardActivityDirection::Outgoing
        ? CLPClipboardDirectionOutgoing
        : CLPClipboardDirectionIncoming;
}

NSDate* ToNSDate(std::chrono::system_clock::time_point timestamp) {
    const auto seconds = std::chrono::duration<double>(timestamp.time_since_epoch()).count();
    return [NSDate dateWithTimeIntervalSince1970:seconds];
}

NSString* ActivityIdentifier(uint64_t itemID) {
    return [NSString stringWithFormat:@"%llu", static_cast<unsigned long long>(itemID)];
}

NSData* DataFromBytes(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) {
        return nil;
    }
    return [NSData dataWithBytes:bytes.data() length:bytes.size()];
}

CLPClipboardActivityItem* MakeClipboardActivityItem(const ClipboardActivityItemHeader& header) {
    const auto display = g_clipboardActivityStore.DisplayItem(header.id);
    if (!display) {
        return nil;
    }

    NSString* detailText = ToNSString(display->detailText);
    return [[CLPClipboardActivityItem alloc] initWithActivityItemID:header.id
                                                        identifier:ActivityIdentifier(header.id)
                                                        deviceName:ToNSString(display->header.deviceName)
                                                         timestamp:ToNSDate(display->header.timestamp)
                                                         direction:ToBridgeDirection(display->header.direction)
                                                              kind:ToBridgePayloadKind(display->kind)
                                                       previewText:ToNSString(display->previewText)
                                                        detailText:detailText
                                                          linkHost:ToNSString(display->linkHost)
                                                              text:detailText
                                                      imagePNGData:DataFromBytes(display->imagePngData)];
}

bool ClipboardPayloadFromPasteboard(ClipboardPayload& payload, NSError** error) {
    payload = {};
    UIPasteboard* pasteboard = UIPasteboard.generalPasteboard;

    NSString* text = pasteboard.string;
    if (text.length == 0 && pasteboard.URL != nil) {
        text = pasteboard.URL.absoluteString;
    }

    if (text.length > 0) {
        NSData* textData = [text dataUsingEncoding:NSUTF8StringEncoding];
        if (textData == nil) {
            AssignError(error, kClippOutgoingClipboardErrorBase + 1, @"Unable to encode clipboard text.");
            return false;
        }

        payload.formatId = CF_UNICODETEXT;
        const auto* bytes = static_cast<const unsigned char*>(textData.bytes);
        payload.rawData.assign(bytes, bytes + textData.length);
        payload.rawData.push_back('\0');
        return true;
    }

    NSData* pngData = [pasteboard dataForPasteboardType:@"public.png"];
    if (pngData.length == 0 && pasteboard.image != nil) {
        pngData = UIImagePNGRepresentation(pasteboard.image);
    }

    if (pngData.length > 0) {
        payload.formatId = CF_DIB;
        const auto* bytes = static_cast<const unsigned char*>(pngData.bytes);
        payload.rawData.assign(bytes, bytes + pngData.length);
        if (!IsPngStream(payload.rawData)) {
            AssignError(error, kClippOutgoingClipboardErrorBase + 2, @"Clipboard image is not a valid PNG.");
            return false;
        }
        return true;
    }

    AssignError(error, kClippOutgoingClipboardErrorBase + 3, @"The clipboard does not contain sendable text or image data.");
    return false;
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

CLPSettingsSnapshot* LoadSettingsSnapshot(NSError** error) {
    HostId hostID;
    if (!g_settings.ensureHostID(hostID)) {
        AssignError(error, kClippSettingsErrorBase + 3, @"Unable to initialize host ID.");
        return nil;
    }

    return [[CLPSettingsSnapshot alloc] initWithClipboardHistoryMemoryLimitBytes:g_settings.clipboardHistoryMemoryLimitBytes()
                                                 clipboardHistoryMaxAgeSeconds:g_settings.clipboardHistoryMaxAgeSeconds()
                                                      clipboardHistoryMaxItems:g_settings.clipboardHistoryMaxItems()
                                                                       tcpPort:g_settings.tcpPort()
                                                                       udpPort:g_settings.mdnsPort()
                                                                    listenerIP:ToNSString(g_settings.listenerIp())
                                                                   multicastIP:ToNSString(g_settings.multicastIp())
                                                                        hostID:ToNSString(hostID.ToHexString())
                                                     hasHostIDCollisionWarning:MDNSHasHostIDCollisionWarning()];
}
}

void CLPIOSReceiveClipboardPayload(const std::wstring& hostName, const ClipboardPayload& payload) {
    @autoreleasepool {
        if (payload.formatId == CF_UNICODETEXT) {
            NSString* text = ClipboardTextFromPayload(payload);
            if (text.length == 0 && payload.rawData.size() != 0) {
                g_logger.log("iOS", Logger::Level::Warning, L"Incoming text clipboard payload could not be decoded as UTF-8.");
                return;
            }
        } else if (payload.formatId == CF_DIB) {
            if (!IsPngStream(payload.rawData)) {
                g_logger.log("iOS", Logger::Level::Warning, L"Incoming image clipboard payload was not a PNG stream (%zu bytes).", payload.rawData.size());
                return;
            }
        } else {
            g_logger.log("iOS", Logger::Level::Warning, L"Unsupported incoming clipboard format ID %u; payload ignored.", payload.formatId);
            return;
        }

        if (!g_clipboardHashGuard.AcceptCurrent(payload)) {
            g_logger.log("iOS", Logger::Level::Debug, L"Ignoring duplicate incoming clipboard payload.");
            return;
        }

        EnsureClipboardActivityWatcher();
        g_clipboardActivityStore.AddIncoming(hostName, payload);
    }
}

@implementation CLPClipboardActivityItem

- (instancetype)initWithActivityItemID:(unsigned long long)activityItemID
                            identifier:(NSString*)identifier
                            deviceName:(NSString*)deviceName
                             timestamp:(NSDate*)timestamp
                             direction:(CLPClipboardDirection)direction
                                  kind:(CLPClipboardPayloadKind)kind
                           previewText:(NSString*)previewText
                            detailText:(NSString*)detailText
                              linkHost:(NSString*)linkHost
                                  text:(NSString*)text
                          imagePNGData:(NSData*)imagePNGData {
    self = [super init];
    if (self) {
        _activityItemID = activityItemID;
        _identifier = [identifier copy];
        _deviceName = [deviceName copy];
        _timestamp = [timestamp copy];
        _direction = direction;
        _kind = kind;
        _previewText = [previewText copy];
        _detailText = [detailText copy];
        _linkHost = [linkHost copy];
        _text = [text copy];
        _imagePNGData = [imagePNGData copy];
    }
    return self;
}

- (BOOL)hasTextPayload {
    return (self.kind == CLPClipboardPayloadKindText ||
            self.kind == CLPClipboardPayloadKindPrivateText ||
            self.kind == CLPClipboardPayloadKindLink) &&
        self.text.length > 0;
}

- (BOOL)hasImagePayload {
    return self.kind == CLPClipboardPayloadKindImage && self.imagePNGData.length > 0;
}

- (BOOL)isIncoming {
    return self.direction == CLPClipboardDirectionIncoming;
}

- (BOOL)isOutgoing {
    return self.direction == CLPClipboardDirectionOutgoing;
}

@end

@implementation CLPClipboardActivityBridge

+ (NSString*)didChangeNotificationName {
    return kClipboardActivityDidChangeNotification;
}

+ (NSArray<CLPClipboardActivityItem*>*)recentItems {
    EnsureClipboardActivityWatcher();
    std::vector<ClipboardActivityItemHeader> snapshot = g_clipboardActivityStore.Snapshot();
    NSMutableArray<CLPClipboardActivityItem*>* items = [NSMutableArray arrayWithCapacity:snapshot.size()];
    for (const ClipboardActivityItemHeader& header : snapshot) {
        CLPClipboardActivityItem* item = MakeClipboardActivityItem(header);
        if (item != nil) {
            [items addObject:item];
        }
    }
    return items;
}

+ (BOOL)copyItem:(CLPClipboardActivityItem*)item
           error:(NSError**)error {
    if (item == nil) {
        AssignError(error, kClippClipboardActivityErrorBase + 1, @"No clipboard item is available to copy.");
        return NO;
    }

    std::optional<ClipboardPayload> payload = g_clipboardActivityStore.PayloadForClipboard(item.activityItemID);
    if (!payload) {
        AssignError(error, kClippClipboardActivityErrorBase + 2, @"The clipboard item is no longer available.");
        return NO;
    }

    if (item.hasTextPayload) {
        NSString* text = ClipboardTextFromPayload(*payload);
        if (text == nil) {
            AssignError(error, kClippClipboardActivityErrorBase + 3, @"Unable to decode clipboard text.");
            return NO;
        }

        UIPasteboard.generalPasteboard.string = text;
        g_clipboardHashGuard.RememberCurrent(*payload);
        return YES;
    }

    if (item.hasImagePayload) {
        UIImage* image = [UIImage imageWithData:item.imagePNGData];
        if (image == nil) {
            AssignError(error, kClippClipboardActivityErrorBase + 4, @"Unable to decode clipboard image.");
            return NO;
        }

        [UIPasteboard.generalPasteboard setData:item.imagePNGData
                              forPasteboardType:@"public.png"];
        g_clipboardHashGuard.RememberCurrent(*payload);
        return YES;
    }

    AssignError(error, kClippClipboardActivityErrorBase + 5, @"Unsupported clipboard item.");
    return NO;
}

@end

@implementation CLPOutgoingClipboardBridge

+ (CLPClipboardActivityItem*)sendCurrentPasteboardWithError:(NSError**)error {
    if (!EnsureSodium(error) || !EnsureHostID(error)) {
        return nil;
    }

    if (!g_keyManager.HaveNetworkKey()) {
        AssignError(error, kClippOutgoingClipboardErrorBase + 4, @"No network key configured.");
        return nil;
    }

    if (!StartNetworkRuntimeIfNeeded(error)) {
        return nil;
    }

    ClipboardPayload payload{};
    if (!ClipboardPayloadFromPasteboard(payload, error)) {
        return nil;
    }

    ClipboardPayload payloadToSend = payload;
    const size_t decodedDataSize = payloadToSend.rawData.size();
    if (!payloadToSend.ZstdCompress()) {
        AssignError(error, kClippOutgoingClipboardErrorBase + 5, @"Unable to prepare clipboard data for sending.");
        return nil;
    }

    if (payload.formatId != CF_UNICODETEXT && payload.formatId != CF_DIB) {
        AssignError(error, kClippOutgoingClipboardErrorBase + 6, @"Unsupported clipboard data.");
        return nil;
    }

    EnsureClipboardActivityWatcher();
    g_clipboardHashGuard.RememberCurrent(payload);
    auto sharedPayload = std::make_shared<const ClipboardPayload>(std::move(payloadToSend));
    const uint64_t activityItemID = g_clipboardActivityStore.AddOutgoing(L"This iPhone", *sharedPayload);
    g_peerManager.BroadcastClipboard(sharedPayload);
    g_logger.log("iOS",
                 Logger::Level::Info,
                 L"Broadcast current iOS pasteboard (format ID: %u, encoded size: %zu bytes, decoded size: %zu bytes)",
                 sharedPayload->formatId,
                 sharedPayload->rawData.size(),
                 decodedDataSize);

    ClipboardActivityItemHeader header{};
    header.id = activityItemID;
    CLPClipboardActivityItem* item = MakeClipboardActivityItem(header);
    if (item == nil) {
        AssignError(error, kClippOutgoingClipboardErrorBase + 7, @"Unable to record sent clipboard item.");
        return nil;
    }
    return item;
}

@end

@implementation CLPNetworkTrafficSnapshot

- (instancetype)initWithBytesSent:(unsigned long long)bytesSent
                    bytesReceived:(unsigned long long)bytesReceived {
    self = [super init];
    if (self) {
        _bytesSent = bytesSent;
        _bytesReceived = bytesReceived;
    }
    return self;
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

+ (CLPNetworkTrafficSnapshot*)trafficSnapshot {
    const auto peers = g_peerDisplay.Query();
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    for (const auto& peer : peers) {
        bytesSent += peer.bytesSent;
        bytesReceived += peer.bytesReceived;
    }

    return [[CLPNetworkTrafficSnapshot alloc] initWithBytesSent:bytesSent
                                                  bytesReceived:bytesReceived];
}

@end

@implementation CLPSettingsSnapshot

- (instancetype)initWithClipboardHistoryMemoryLimitBytes:(unsigned long long)clipboardHistoryMemoryLimitBytes
                         clipboardHistoryMaxAgeSeconds:(unsigned long long)clipboardHistoryMaxAgeSeconds
                              clipboardHistoryMaxItems:(unsigned long long)clipboardHistoryMaxItems
                                               tcpPort:(NSInteger)tcpPort
                                               udpPort:(NSInteger)udpPort
                                            listenerIP:(NSString*)listenerIP
                                           multicastIP:(NSString*)multicastIP
                                                hostID:(NSString*)hostID
                             hasHostIDCollisionWarning:(BOOL)hasHostIDCollisionWarning {
    self = [super init];
    if (self) {
        _clipboardHistoryMemoryLimitBytes = clipboardHistoryMemoryLimitBytes;
        _clipboardHistoryMaxAgeSeconds = clipboardHistoryMaxAgeSeconds;
        _clipboardHistoryMaxItems = clipboardHistoryMaxItems;
        _tcpPort = tcpPort;
        _udpPort = udpPort;
        _listenerIP = [listenerIP copy];
        _multicastIP = [multicastIP copy];
        _hostID = [hostID copy];
        _hasHostIDCollisionWarning = hasHostIDCollisionWarning;
    }
    return self;
}

@end

@implementation CLPSettingsBridge

+ (CLPSettingsSnapshot*)loadSnapshotWithError:(NSError**)error {
    EnsureClipboardHistoryLimitsApplied();
    return LoadSettingsSnapshot(error);
}

+ (CLPSettingsSnapshot*)updateClipboardHistoryMemoryLimitBytes:(unsigned long long)memoryLimitBytes
                                                 maxAgeSeconds:(unsigned long long)maxAgeSeconds
                                                      maxItems:(unsigned long long)maxItems
                                                         error:(NSError**)error {
    bool changed = false;
    if (memoryLimitBytes != g_settings.clipboardHistoryMemoryLimitBytes()) {
        if (!g_settings.set_clipboardHistoryMemoryLimitBytes(memoryLimitBytes)) {
            AssignError(error, kClippSettingsErrorBase + 4, @"Unable to save clipboard history memory limit.");
            return nil;
        }
        changed = true;
    }
    if (maxAgeSeconds != g_settings.clipboardHistoryMaxAgeSeconds()) {
        if (!g_settings.set_clipboardHistoryMaxAgeSeconds(maxAgeSeconds)) {
            AssignError(error, kClippSettingsErrorBase + 5, @"Unable to save clipboard history time limit.");
            return nil;
        }
        changed = true;
    }
    if (maxItems != g_settings.clipboardHistoryMaxItems()) {
        if (!g_settings.set_clipboardHistoryMaxItems(maxItems)) {
            AssignError(error, kClippSettingsErrorBase + 6, @"Unable to save clipboard history item limit.");
            return nil;
        }
        changed = true;
    }

    if (changed) {
        ApplyClipboardHistoryLimitsFromSettings();
    } else {
        EnsureClipboardHistoryLimitsApplied();
    }
    return LoadSettingsSnapshot(error);
}

+ (CLPSettingsSnapshot*)updateNetworkTcpPort:(NSInteger)tcpPort
                                     udpPort:(NSInteger)udpPort
                                  listenerIP:(NSString*)listenerIP
                                 multicastIP:(NSString*)multicastIP
                                       error:(NSError**)error {
    if (!Settings::IsValidPort(static_cast<int>(tcpPort))) {
        AssignError(error, kClippSettingsErrorBase + 7, @"TCP port must be between 1 and 65535.");
        return nil;
    }
    if (!Settings::IsValidPort(static_cast<int>(udpPort))) {
        AssignError(error, kClippSettingsErrorBase + 8, @"UDP port must be between 1 and 65535.");
        return nil;
    }

    const std::string listenerIPValue = uiSettingsPage::TrimAscii(ToStdString(listenerIP));
    if (!Settings::IsValidListenerIp(listenerIPValue)) {
        AssignError(error, kClippSettingsErrorBase + 9, @"Listener IP must be a valid IPv4 address.");
        return nil;
    }

    const std::string multicastIPValue = uiSettingsPage::TrimAscii(ToStdString(multicastIP));
    if (!Settings::IsValidMulticastIp(multicastIPValue)) {
        AssignError(error, kClippSettingsErrorBase + 10, @"Multicast IP must be a valid multicast IPv4 address.");
        return nil;
    }

    bool changed = false;
    if (static_cast<int>(tcpPort) != g_settings.tcpPort()) {
        if (!g_settings.set_tcpPort(static_cast<int>(tcpPort))) {
            AssignError(error, kClippSettingsErrorBase + 11, @"Unable to save TCP port.");
            return nil;
        }
        changed = true;
    }
    if (static_cast<int>(udpPort) != g_settings.mdnsPort()) {
        if (!g_settings.set_mdnsPort(static_cast<int>(udpPort))) {
            AssignError(error, kClippSettingsErrorBase + 12, @"Unable to save UDP port.");
            return nil;
        }
        changed = true;
    }
    if (listenerIPValue != g_settings.listenerIp()) {
        if (!g_settings.set_listenerIp(listenerIPValue)) {
            AssignError(error, kClippSettingsErrorBase + 13, @"Unable to save listener IP.");
            return nil;
        }
        changed = true;
    }
    if (multicastIPValue != g_settings.multicastIp()) {
        if (!g_settings.set_multicastIp(multicastIPValue)) {
            AssignError(error, kClippSettingsErrorBase + 14, @"Unable to save multicast IP.");
            return nil;
        }
        changed = true;
    }

    if (changed) {
        if (!RestartNetworkRuntime(error)) {
            return nil;
        }
    }

    return LoadSettingsSnapshot(error);
}

+ (CLPSettingsSnapshot*)resetHostIDWithError:(NSError**)error {
    HostId hostID;
    if (!g_settings.resetHostID(hostID)) {
        AssignError(error, kClippSettingsErrorBase + 15, @"Unable to reset Host ID.");
        return nil;
    }

    MDNSNotifyHostIDChange();
    g_peerManager.ClearPeers();
    return LoadSettingsSnapshot(error);
}

@end
