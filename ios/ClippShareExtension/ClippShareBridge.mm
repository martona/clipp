#import "ClippShareBridge.h"

#include "../../src/ClipboardData.h"
#include "../../src/CryptoChannel.h"
#include "../../src/KeyManager.h"
#include "../../src/Logger.h"
#include "../../src/NetworkDefs.h"
#include "../../src/Settings.h"
#include "../../src/platform.h"
#include "../../src/utils_socket.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>

extern Settings g_settings;
extern KeyManager g_keyManager;

namespace {
constexpr NSInteger kClippShareErrorBase = 5100;
constexpr auto kDiscoveryWait = std::chrono::milliseconds(1200);
constexpr auto kConnectWait = std::chrono::seconds(3);
constexpr auto kSendWait = std::chrono::seconds(30);
constexpr const char* kDiscoveryProtocolSelector = "clipp";
constexpr int kDiscoveryProtocolVersion = 1;

struct DiscoveredPeer {
    std::string hostName;
    std::string ip;
    HostId hostID;
    unsigned short port = 0;
};

struct ShareDiscoveryPacket {
    ShareDiscoveryPacket() {
        std::memset(this, 0, sizeof(*this));
    }

    char selector[16];
    u_short version;
    char hostName[256];
    unsigned char hostID[32];
    u_short port;
    char verb[16];
    unsigned char queryID[32];
    unsigned char nonce[32];
};

struct EncryptedShareDiscoveryPacket {
    unsigned char nonce[crypto_secretbox_NONCEBYTES];
    unsigned char ciphertext[sizeof(ShareDiscoveryPacket) + crypto_secretbox_MACBYTES];
};

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

bool IsPngData(NSData* data) {
    static constexpr unsigned char signature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    return data.length >= sizeof(signature)
        && std::memcmp(data.bytes, signature, sizeof(signature)) == 0;
}

bool PayloadFromSharePayload(CLPSharePayload* sharePayload, ClipboardPayload& payload) {
    payload = {};

    if (sharePayload.kind == CLPSharePayloadKindText) {
        const std::string text = ToStdString(sharePayload.text);
        if (text.empty()) {
            return false;
        }

        payload.formatId = CF_UNICODETEXT;
        payload.rawData.assign(text.begin(), text.end());
        payload.rawData.push_back('\0');
        return payload.ZstdCompress();
    }

    if (sharePayload.kind == CLPSharePayloadKindPNG) {
        NSData* pngData = sharePayload.pngData;
        if (pngData.length == 0 || !IsPngData(pngData)) {
            return false;
        }

        payload.formatId = CF_DIB;
        const auto* bytes = static_cast<const unsigned char*>(pngData.bytes);
        payload.rawData.assign(bytes, bytes + pngData.length);
        return payload.ZstdCompress();
    }

    return false;
}

bool GetMDNSKey(KeyManager::NetworkKey& key) {
    std::string errorMessage;
    return g_keyManager.GetKey(KeyManager::KeyRole::MDNS, key, &errorMessage);
}

ShareDiscoveryPacket BuildShareDiscoveryPacket(const std::string& hostName,
                                               const HostId& localHostID,
                                               const unsigned char* queryID) {
    ShareDiscoveryPacket packet;
    packet.version = htons(kDiscoveryProtocolVersion);
    randombytes_buf(packet.nonce, sizeof(packet.nonce));
    strncpys(packet.selector, kDiscoveryProtocolSelector);
    strncpys(packet.hostName, hostName.c_str());
    strncpys(packet.verb, "query");
    packet.port = htons(static_cast<u_short>(g_settings.tcpPort()));
    std::memcpy(packet.hostID, localHostID.data().data(), sizeof(packet.hostID));
    std::memcpy(packet.queryID, queryID, sizeof(packet.queryID));
    return packet;
}

bool EncryptShareDiscoveryPacket(const ShareDiscoveryPacket& packet, EncryptedShareDiscoveryPacket& encryptedPacket) {
    KeyManager::NetworkKey mdnsKey{};
    if (!GetMDNSKey(mdnsKey)) {
        return false;
    }

    randombytes_buf(encryptedPacket.nonce, sizeof(encryptedPacket.nonce));
    return crypto_secretbox_easy(
        encryptedPacket.ciphertext,
        reinterpret_cast<const unsigned char*>(&packet),
        sizeof(packet),
        encryptedPacket.nonce,
        mdnsKey.data()) == 0;
}

bool DecryptShareDiscoveryPacket(const char* packet, size_t packetLen, ShareDiscoveryPacket& decryptedPacket) {
    if (packet == nullptr || packetLen != sizeof(EncryptedShareDiscoveryPacket)) {
        return false;
    }

    KeyManager::NetworkKey mdnsKey{};
    if (!GetMDNSKey(mdnsKey)) {
        return false;
    }

    const auto* encryptedPacket = reinterpret_cast<const EncryptedShareDiscoveryPacket*>(packet);
    return crypto_secretbox_open_easy(
        reinterpret_cast<unsigned char*>(&decryptedPacket),
        encryptedPacket->ciphertext,
        sizeof(encryptedPacket->ciphertext),
        encryptedPacket->nonce,
        mdnsKey.data()) == 0;
}

bool ParseShareDiscoveryResponse(ShareDiscoveryPacket& packet,
                                 const unsigned char* expectedQueryID,
                                 const HostId& localHostID,
                                 DiscoveredPeer& peer) {
    if (std::strncmp(packet.selector, kDiscoveryProtocolSelector, cntof(packet.selector)) != 0
        || packet.version != htons(kDiscoveryProtocolVersion)) {
        return false;
    }

    packet.hostName[cntof(packet.hostName) - 1] = 0;
    packet.verb[cntof(packet.verb) - 1] = 0;
    if (packet.hostName[0] == 0 || std::strcmp(packet.verb, "response") != 0) {
        return false;
    }

    if (std::memcmp(packet.queryID, expectedQueryID, sizeof(packet.queryID)) != 0) {
        return false;
    }

    HostId remoteHostID(packet.hostID);
    const unsigned short port = ntohs(packet.port);
    if (remoteHostID == localHostID || port == 0) {
        return false;
    }

    peer.hostName = packet.hostName;
    peer.hostID = remoteHostID;
    peer.port = port;
    return true;
}

bool AddUniquePeer(std::vector<DiscoveredPeer>& peers, DiscoveredPeer peer) {
    const auto found = std::find_if(peers.begin(), peers.end(), [&](const DiscoveredPeer& existingPeer) {
        return existingPeer.hostID == peer.hostID;
    });
    if (found != peers.end()) {
        return false;
    }

    peers.push_back(std::move(peer));
    return true;
}

std::vector<DiscoveredPeer> DiscoverPeers(NSError** error) {
    std::vector<DiscoveredPeer> peers;

    HostId localHostID;
    if (!g_settings.getHostID(localHostID)) {
        AssignError(error, kClippShareErrorBase + 4, @"Unable to start local network discovery.");
        return {};
    }

    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        AssignError(error, kClippShareErrorBase + 4, @"Unable to start local network discovery.");
        return {};
    }

    int reuseAddr = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = 0;
    if (inet_pton(AF_INET, g_settings.listenerIp().c_str(), &bindAddr.sin_addr) != 1
        || bind(socket, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(socket);
        AssignError(error, kClippShareErrorBase + 4, @"Unable to start local network discovery.");
        return {};
    }

    sockaddr_in multicastAddr{};
    multicastAddr.sin_family = AF_INET;
    multicastAddr.sin_port = htons(static_cast<u_short>(g_settings.mdnsPort()));
    if (inet_pton(AF_INET, g_settings.multicastIp().c_str(), &multicastAddr.sin_addr) != 1) {
        closesocket(socket);
        AssignError(error, kClippShareErrorBase + 4, @"Unable to start local network discovery.");
        return {};
    }

    std::array<unsigned char, 32> queryID{};
    randombytes_buf(queryID.data(), queryID.size());

    char localHostName[256] = {};
    if (gethostname(localHostName, sizeof(localHostName)) != 0) {
        strncpys(localHostName, "iPhone");
    }

    ShareDiscoveryPacket packet = BuildShareDiscoveryPacket(localHostName, localHostID, queryID.data());
    EncryptedShareDiscoveryPacket encryptedPacket{};
    if (!EncryptShareDiscoveryPacket(packet, encryptedPacket)) {
        closesocket(socket);
        AssignError(error, kClippShareErrorBase + 4, @"Unable to start local network discovery.");
        return {};
    }

    const auto sent = sendto(socket,
                             reinterpret_cast<const char*>(&encryptedPacket),
                             sizeof(encryptedPacket),
                             0,
                             reinterpret_cast<const sockaddr*>(&multicastAddr),
                             sizeof(multicastAddr));
    if (sent < 0 || static_cast<size_t>(sent) != sizeof(encryptedPacket)) {
        closesocket(socket);
        AssignError(error, kClippShareErrorBase + 4, @"Unable to start local network discovery.");
        return {};
    }

    const auto deadline = std::chrono::steady_clock::now() + kDiscoveryWait;
    std::array<char, sizeof(EncryptedShareDiscoveryPacket)> recvBuffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        const auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(remaining);

        timeval timeout{};
        timeout.tv_sec = static_cast<long>(remainingUs.count() / 1000000);
        timeout.tv_usec = static_cast<suseconds_t>(remainingUs.count() % 1000000);

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket, &readSet);

        const int ready = select(static_cast<int>(socket) + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready < 0) {
            break;
        }
        if (ready == 0 || !FD_ISSET(socket, &readSet)) {
            continue;
        }

        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        const auto bytesRead = recvfrom(socket,
                                        recvBuffer.data(),
                                        static_cast<int>(recvBuffer.size()),
                                        0,
                                        reinterpret_cast<sockaddr*>(&fromAddr),
                                        &fromLen);
        if (bytesRead <= 0) {
            continue;
        }

        ShareDiscoveryPacket decryptedPacket;
        if (!DecryptShareDiscoveryPacket(recvBuffer.data(), static_cast<size_t>(bytesRead), decryptedPacket)) {
            continue;
        }

        DiscoveredPeer peer;
        if (!ParseShareDiscoveryResponse(decryptedPacket, queryID.data(), localHostID, peer)) {
            continue;
        }

        char senderIp[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &fromAddr.sin_addr, senderIp, sizeof(senderIp)) == nullptr) {
            continue;
        }
        peer.ip = senderIp;
        AddUniquePeer(peers, std::move(peer));
    }

    closesocket(socket);
    return peers;
}

bool SetSocketNonblocking(SOCKET socket) {
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool IsConnectPending(int error) {
    return error == EINPROGRESS || error == EWOULDBLOCK || error == EALREADY;
}

SOCKET ConnectToPeer(const DiscoveredPeer& peer) {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    if (!SetSocketNonblocking(socket)) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(peer.port);
    if (inet_pton(AF_INET, peer.ip.c_str(), &address.sin_addr) != 1) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        return socket;
    }

    if (!IsConnectPending(errno)) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    fd_set writeSet;
    fd_set errorSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    FD_SET(socket, &writeSet);
    FD_SET(socket, &errorSet);

    timeval timeout{};
    timeout.tv_sec = static_cast<long>(kConnectWait.count());

    const int ready = select(static_cast<int>(socket) + 1, nullptr, &writeSet, &errorSet, &timeout);
    if (ready <= 0) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    int connectError = 0;
    socklen_t connectErrorLen = sizeof(connectError);
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connectError), &connectErrorLen) != 0
        || connectError != 0) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    return socket;
}

bool SendClipboardData(CryptoChannel& channel, const SocketIoContext& io, const ClipboardPayload& payload) {
    if (payload.rawData.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    NetworkDefs::ClipboardMessage message{};
    message.formatId = htonl(payload.formatId);
    message.isCompressed = payload.isCompressed ? 1 : 0;
    message.encodedDataSize = htonl(static_cast<uint32_t>(payload.rawData.size()));
    message.decodedDataSize = htonl(payload.decodedDataSize);

    if (io.socket == INVALID_SOCKET || !channel.SendTaggedMessage(io, "CLIP")) {
        return false;
    }
    if (!channel.SendMessage(io, reinterpret_cast<unsigned char*>(&message), sizeof(message))) {
        return false;
    }
    if (!payload.rawData.empty()
        && !channel.SendMessage(io, payload.rawData.data(), static_cast<uint32_t>(payload.rawData.size()))) {
        return false;
    }
    return true;
}

bool SendPayloadsToPeer(const DiscoveredPeer& peer, const std::vector<ClipboardPayload>& payloads) {
    SOCKET socket = ConnectToPeer(peer);
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
            if (!SendClipboardData(channel, io, payload)) {
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
                     pngData:(nullable NSData*)pngData NS_DESIGNATED_INITIALIZER;

@end

@implementation CLPSharePayload

+ (instancetype)textPayloadWithText:(NSString*)text {
    CLPSharePayload* payload = [[CLPSharePayload alloc] initWithKind:CLPSharePayloadKindText
                                                                text:text
                                                             pngData:nil];
    return payload;
}

+ (instancetype)pngPayloadWithData:(NSData*)pngData {
    CLPSharePayload* payload = [[CLPSharePayload alloc] initWithKind:CLPSharePayloadKindPNG
                                                                text:nil
                                                             pngData:pngData];
    return payload;
}

- (instancetype)initWithKind:(CLPSharePayloadKind)kind
                        text:(NSString*)text
                     pngData:(NSData*)pngData {
    self = [super init];
    if (self) {
        _kind = kind;
        _text = [text copy];
        _pngData = [pngData copy];
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

    NSError* discoveryError = nil;
    std::vector<DiscoveredPeer> peers = DiscoverPeers(&discoveryError);
    if (peers.empty()) {
        if (discoveryError != nil && error != nullptr) {
            *error = discoveryError;
        } else {
            AssignError(error, kClippShareErrorBase + 6, @"No trusted devices were found on the local network.");
        }
        return nil;
    }

    NSInteger reachedDevices = 0;
    for (const DiscoveredPeer& peer : peers) {
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
