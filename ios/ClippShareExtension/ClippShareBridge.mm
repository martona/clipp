#import "ClippShareBridge.h"

#include "../../src/ClipboardWire.h"
#include "../../src/ClipboardPayload.h"
#include "../../src/CryptoChannel.h"
#include "../../src/KeyManager.h"
#include "../../src/LocalPeerName.h"
#include "../../src/Logger.h"
#include "../../src/MDNSDiscovery.h"
#include "../../src/MDNSProtocol.h"
#include "../../src/Settings.h"
#include "../../src/platform.h"
#include "../../src/utils_socket.h"

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <signal.h>
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

bool SendPayloadsToPeer(const MDNSDiscovery::DiscoveredPeer& peer, const std::vector<ClipboardPayload>& payloads) {
    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Share extension connecting to peer %hs at %hs:%hu.",
        peer.deviceName.c_str(),
        peer.ip.c_str(),
        peer.port);

    SOCKET socket = ConnectTcpSocket(peer.ip, peer.port, kConnectWait);
    if (socket == INVALID_SOCKET) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Share extension failed to connect to peer %hs at %hs:%hu.",
            peer.deviceName.c_str(),
            peer.ip.c_str(),
            peer.port);
        return false;
    }

    SocketWakeEvent wakeEvent;
    if (!wakeEvent.Initialize()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Share extension failed to initialize send wake socket for peer %hs.", peer.deviceName.c_str());
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
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Share extension send to peer %hs timed out.", peer.deviceName.c_str());
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

        const std::string localHostName = clipp::GetLocalPeerDisplayName("iPhone", CryptoChannel::HOSTNAME_MAX_BYTES);

        CryptoChannel channel;
        HostId remoteHostID;
        std::string remoteHostName;
        if (!channel.ClientHandshake(io, localHostID, localHostName, remoteHostID, remoteHostName)) {
            g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Share extension handshake failed for peer %hs.", peer.deviceName.c_str());
            break;
        }

        if (remoteHostID != peer.hostId) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Share extension peer identity mismatch for %hs.", peer.deviceName.c_str());
            break;
        }

        bool sentAll = true;
        for (const ClipboardPayload& payload : payloads) {
            if (!ClipboardWire::SendClipboardPayload(channel, io, payload)) {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Share extension failed to send %ls payload to peer %hs.",
                    ClippClipboardFormatNameW(payload.meta.formatId),
                    peer.deviceName.c_str());
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
    g_logger.log(__FUNCTION__, sent ? Logger::Level::Debug : Logger::Level::Warning, L"Share extension send to peer %hs %ls.",
        peer.deviceName.c_str(),
        sent ? L"succeeded" : L"failed");
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
                   reachedDeviceCount:(NSInteger)reachedDeviceCount
                  attemptedDeviceCount:(NSInteger)attemptedDeviceCount
                     failedDeviceNames:(NSArray<NSString*>*)failedDeviceNames {
    self = [super init];
    if (self) {
        _sentItemCount = sentItemCount;
        _reachedDeviceCount = reachedDeviceCount;
        _attemptedDeviceCount = attemptedDeviceCount;
        _failedDeviceNames = [failedDeviceNames copy];
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

    std::vector<ClipboardPayload> clipboardPayloads;
    clipboardPayloads.reserve(payloads.count);
    for (CLPSharePayload* payload in payloads) {
        ClipboardPayload clipboardPayload{};
        if (PayloadFromSharePayload(payload, clipboardPayload)) {
            ClipboardWire::FinalizeOutgoingPayload(clipboardPayload, localHostId);
            clipboardPayloads.push_back(std::move(clipboardPayload));
        }
    }

    if (clipboardPayloads.empty()) {
        AssignError(error, kClippShareErrorBase + 5, @"No supported shared items could be prepared.");
        return nil;
    }

    std::vector<MDNSDiscovery::DiscoveredPeer> peers;
    if (!MDNSDiscovery::BrowseOnce(kDiscoveryWait, peers)) {
        AssignError(error, kClippShareErrorBase + 4, @"Unable to start local network discovery.");
        return nil;
    }

    if (peers.empty()) {
        AssignError(error, kClippShareErrorBase + 6, @"No trusted devices were found on the local network.");
        return nil;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Share extension discovered %zu peer(s).", peers.size());

    std::atomic<int> reachedDevices{ 0 };
    std::mutex failedPeersMutex;
    std::vector<std::string> failedPeerNames;
    std::vector<std::thread> sendThreads;
    sendThreads.reserve(peers.size());
    for (const MDNSDiscovery::DiscoveredPeer& peer : peers) {
        sendThreads.emplace_back([&clipboardPayloads, peer, &reachedDevices, &failedPeersMutex, &failedPeerNames]() {
            if (SendPayloadsToPeer(peer, clipboardPayloads)) {
                ++reachedDevices;
            } else {
                std::lock_guard<std::mutex> lock(failedPeersMutex);
                failedPeerNames.push_back(peer.deviceName);
            }
        });
    }

    for (std::thread& sendThread : sendThreads) {
        if (sendThread.joinable()) {
            sendThread.join();
        }
    }

    const int reachedDeviceCount = reachedDevices.load();
    NSMutableArray<NSString*>* failedDeviceNames = [NSMutableArray arrayWithCapacity:failedPeerNames.size()];
    for (const std::string& failedPeerName : failedPeerNames) {
        NSString* name = [NSString stringWithUTF8String:failedPeerName.c_str()];
        [failedDeviceNames addObject:name ?: @"Unknown device"];
    }

    if (reachedDeviceCount == 0) {
        if (failedDeviceNames.count > 0) {
            NSString* failedList = [failedDeviceNames componentsJoinedByString:@", "];
            AssignError(error, kClippShareErrorBase + 7, [NSString stringWithFormat:@"No trusted devices accepted the shared items. Failed: %@.", failedList]);
        } else {
            AssignError(error, kClippShareErrorBase + 7, @"Trusted devices were found, but none accepted the shared items.");
        }
        return nil;
    }

    return [[CLPShareSendResult alloc] initWithSentItemCount:static_cast<NSInteger>(clipboardPayloads.size())
                                         reachedDeviceCount:static_cast<NSInteger>(reachedDeviceCount)
                                        attemptedDeviceCount:static_cast<NSInteger>(peers.size())
                                           failedDeviceNames:failedDeviceNames];
}

@end
