#import "ClippShareBridge.h"

#include "../../src/ClipboardWire.h"
#include "../../src/ClipboardData.h"
#include "../../src/CryptoChannel.h"
#include "../../src/KeyManager.h"
#include "../../src/Logger.h"
#include "../../src/MDNSProtocol.h"
#include "../../src/Settings.h"
#include "../../src/platform.h"
#include "../../src/utils_socket.h"

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern Settings g_settings;
extern KeyManager g_keyManager;

namespace {
constexpr NSInteger kClippShareErrorBase = 5100;
constexpr auto kDiscoveryWait = std::chrono::milliseconds(1200);
constexpr auto kConnectWait = std::chrono::seconds(3);
constexpr auto kSendWait = std::chrono::seconds(30);

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
    payload = {};

    if (sharePayload.kind == CLPSharePayloadKindText) {
        const std::string text = ToStdString(sharePayload.text);
        if (text.empty()) {
            return false;
        }

        payload.formatId = CLIPP_FORMAT_UTF8;
        payload.rawData.assign(text.begin(), text.end());
        payload.rawData.push_back('\0');
        return payload.ZstdCompress();
    }

    if (sharePayload.kind == CLPSharePayloadKindJPEG) {
        NSData* jpegData = sharePayload.jpegData;
        if (jpegData.length == 0) {
            return false;
        }

        payload.formatId = CLIPP_FORMAT_JPEG;
        const auto* bytes = static_cast<const unsigned char*>(jpegData.bytes);
        payload.rawData.assign(bytes, bytes + jpegData.length);
        return payload.ZstdCompress();
    }

    if (sharePayload.kind == CLPSharePayloadKindPNG) {
        NSData* pngData = sharePayload.pngData;
        if (pngData.length == 0) {
            return false;
        }

        payload.formatId = CLIPP_FORMAT_PNG;
        const auto* bytes = static_cast<const unsigned char*>(pngData.bytes);
        payload.rawData.assign(bytes, bytes + pngData.length);
        return payload.ZstdCompress();
    }

    return false;
}

bool SendPayloadsToPeer(const MDNSProtocol::DiscoveredPeer& peer, const std::vector<ClipboardPayload>& payloads) {
    SOCKET socket = ConnectTcpSocket(peer.ip, peer.port, kConnectWait);
    if (socket == INVALID_SOCKET) {
        return false;
    }

    SocketWakeEvent wakeEvent;
    if (!wakeEvent.Initialize()) {
        closesocket(socket);
        return false;
    }

    std::atomic<bool> stopRequested{ false };
    std::mutex deadlineMutex;
    std::condition_variable deadlineCV;
    bool finished = false;
    std::thread deadlineThread([&]() {
        std::unique_lock<std::mutex> lock(deadlineMutex);
        if (!deadlineCV.wait_for(lock, kSendWait, [&]() { return finished; })) {
            stopRequested = true;
            wakeEvent.Signal();
        }
    });

    bool sent = false;
    do {
        SocketIoContext io{ socket, wakeEvent, stopRequested };

        HostId localHostID;
        if (!g_settings.getHostID(localHostID)) {
            break;
        }

        char localHostName[CryptoChannel::HOSTNAME_MAX_BYTES] = {};
        if (gethostname(localHostName, sizeof(localHostName)) != 0) {
            break;
        }

        CryptoChannel channel;
        HostId remoteHostID;
        std::string remoteHostName;
        if (!channel.ClientHandshake(io, localHostID, localHostName, remoteHostID, remoteHostName)) {
            break;
        }

        if (remoteHostID != peer.hostID) {
            break;
        }

        bool sentAll = true;
        for (const ClipboardPayload& payload : payloads) {
            if (!ClipboardWire::SendClipboardPayload(channel, io, payload)) {
                sentAll = false;
                break;
            }
        }

        sent = sentAll;
    } while (false);

    {
        std::lock_guard<std::mutex> lock(deadlineMutex);
        finished = true;
    }
    deadlineCV.notify_one();
    if (deadlineThread.joinable()) {
        deadlineThread.join();
    }

    shutdown(socket, SD_BOTH);
    closesocket(socket);
    wakeEvent.Close();
    return sent;
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
                   reachedDeviceCount:(NSInteger)reachedDeviceCount {
    self = [super init];
    if (self) {
        _sentItemCount = sentItemCount;
        _reachedDeviceCount = reachedDeviceCount;
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

    if (!g_keyManager.HaveNetworkKey()) {
        AssignError(error, kClippShareErrorBase + 3, @"No network key is configured. Open Clipp to finish setup.");
        return nil;
    }

    std::vector<ClipboardPayload> clipboardPayloads;
    clipboardPayloads.reserve(payloads.count);
    for (CLPSharePayload* payload in payloads) {
        ClipboardPayload clipboardPayload{};
        if (PayloadFromSharePayload(payload, clipboardPayload)) {
            clipboardPayloads.push_back(std::move(clipboardPayload));
        }
    }

    if (clipboardPayloads.empty()) {
        AssignError(error, kClippShareErrorBase + 5, @"No supported shared items could be prepared.");
        return nil;
    }

    std::vector<MDNSProtocol::DiscoveredPeer> peers;
    if (!MDNSProtocol::ProbePeersOnce(kDiscoveryWait, peers)) {
        AssignError(error, kClippShareErrorBase + 4, @"Unable to start local network discovery.");
        return nil;
    }

    if (peers.empty()) {
        AssignError(error, kClippShareErrorBase + 6, @"No trusted devices were found on the local network.");
        return nil;
    }

    NSInteger reachedDevices = 0;
    for (const MDNSProtocol::DiscoveredPeer& peer : peers) {
        if (SendPayloadsToPeer(peer, clipboardPayloads)) {
            ++reachedDevices;
        }
    }

    if (reachedDevices == 0) {
        AssignError(error, kClippShareErrorBase + 7, @"Trusted devices were found, but none accepted the shared items.");
        return nil;
    }

    return [[CLPShareSendResult alloc] initWithSentItemCount:static_cast<NSInteger>(clipboardPayloads.size())
                                         reachedDeviceCount:reachedDevices];
}

@end
