#include "platform.h"
#include "MDNSThread.h"
#include "Settings.h"
#include "KeyManager.h"
#include "Logger.h"
#include <sodium.h>
#include <thread>
#include <future>
#include <chrono>
#include <cstring>
#include <string>
#include <array>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <mutex>

#include "utils_socket.h"
#include "HostId.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Crypt32.lib")

static std::thread g_mdnsThread;
static MDNSCallback g_mdnsCallback = nullptr;
static std::atomic<bool> g_mdnsRunning{ false };
static SOCKET g_mdnsSock = INVALID_SOCKET;
static SocketWakeEvent g_mdnsWakeEvent;
static std::mutex g_mdnsSocketMutex;
static std::atomic<bool> g_mdnsSendImmediately{ false };
static std::array<unsigned char, 32> g_lastSentQueryID{};
static HostId g_hostId;

struct mdns_packet {
    mdns_packet() {
		memset(this, 0, sizeof(*this));
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

struct encrypted_mdns_packet {
    unsigned char nonce[crypto_secretbox_NONCEBYTES];
    unsigned char ciphertext[sizeof(mdns_packet) + crypto_secretbox_MACBYTES];
};

namespace {
    constexpr const char* kProtocolSelector = "clipp";
    constexpr int kProtocolVersion = 1;
    constexpr auto kBroadcastInterval = std::chrono::minutes(1);
}

static bool HasNetworkKey() {
    std::array<unsigned char, KeyManager::NetworkKeySize> mdnsKey{};
    std::string errorMessage;
    return g_keyManager.GetKey(KeyManager::KeyRole::MDNS, mdnsKey, &errorMessage);
}

static bool EncryptPacket(const mdns_packet& packet, encrypted_mdns_packet& encryptedPacket) {
    std::array<unsigned char, KeyManager::NetworkKeySize> mdnsKey{};
    std::string errorMessage;
    if (!g_keyManager.GetKey(KeyManager::KeyRole::MDNS, mdnsKey, &errorMessage))
        return false;

    randombytes_buf(encryptedPacket.nonce, sizeof(encryptedPacket.nonce));
    return crypto_secretbox_easy(
        encryptedPacket.ciphertext,
        reinterpret_cast<const unsigned char*>(&packet),
        sizeof(packet),
        encryptedPacket.nonce,
        mdnsKey.data()) == 0;
}

static bool DecryptPacket(const char* packet, size_t packetLen, mdns_packet& decryptedPacket) {
    if (!packet || packetLen != sizeof(encrypted_mdns_packet))
        return false;

    const encrypted_mdns_packet* encryptedPacket = reinterpret_cast<const encrypted_mdns_packet*>(packet);
    std::array<unsigned char, KeyManager::NetworkKeySize> mdnsKey{};
    std::string errorMessage;
    if (!g_keyManager.GetKey(KeyManager::KeyRole::MDNS, mdnsKey, &errorMessage))
        return false;

    return crypto_secretbox_open_easy(
        reinterpret_cast<unsigned char*>(&decryptedPacket),
        encryptedPacket->ciphertext,
        sizeof(encryptedPacket->ciphertext),
        encryptedPacket->nonce,
        mdnsKey.data()) == 0;
}

static std::string GetLocalHostName() {
    char hostName[256] = {};
	if (gethostname(hostName, sizeof(hostName)) == 0) {
        return hostName;
    }
    return "unknown";
}

static mdns_packet BuildMDNSPacket(const std::string& hostName, const std::string& verb, const unsigned char* queryID = nullptr) {
    mdns_packet packet;
    packet.version = htons(kProtocolVersion);
    randombytes_buf(packet.nonce, sizeof(packet.nonce));
    strncpys(packet.selector, kProtocolSelector);
    strncpys(packet.hostName, hostName.c_str());
    strncpys(packet.verb, verb.c_str());
    packet.port = htons(static_cast<u_short>(g_settings.tcpPort()));
    std::memcpy(packet.hostID, g_hostId.data().data(), sizeof(packet.hostID));
    if (queryID) {
        std::memcpy(packet.queryID, queryID, sizeof(packet.queryID));
        std::memcpy(g_lastSentQueryID.data(), queryID, sizeof(packet.queryID));
    } else {
        randombytes_buf(packet.queryID, sizeof(packet.queryID));
        std::memcpy(g_lastSentQueryID.data(), packet.queryID, sizeof(packet.queryID));
    }
    return packet;
}

static bool ParseDiscoveryPacket(mdns_packet& pkt, 
                                std::string& hostName, 
                                std::string& verb, 
                                std::string& queryID, 
                                std::string& nonce, 
                                unsigned short& hostPort, 
                                const unsigned char** rawQueryID, 
                                HostId& remoteHostID) 
{
    // Validate selector
    if (strncmp(pkt.selector, kProtocolSelector, cntof(pkt.selector)) != 0)
        return false;
    // Validate version
    if (pkt.version != htons(kProtocolVersion))
        return false;
    // Validate hostName and verb are not empty
    if (pkt.hostName[0] == 0 || pkt.verb[0] == 0)
        return false;
	// Force null-termination of string fields
	pkt.hostName[cntof(pkt.hostName) - 1] = 0;
	pkt.verb[cntof(pkt.verb) - 1] = 0;
    hostName = pkt.hostName;
    verb = pkt.verb;
    if (rawQueryID)
        *rawQueryID = pkt.queryID;
	remoteHostID = pkt.hostID;

    if (verb == "response" && std::memcmp(pkt.queryID, g_lastSentQueryID.data(), sizeof(pkt.queryID)) != 0)
        return false;

	hostPort = ntohs(pkt.port);

    // Convert queryID and nonce to hex wstring
    std::ostringstream ossQueryID, ossNonce;
    for (int i = 0; i < 32; ++i) {
        ossQueryID << std::hex << std::setw(2) << std::setfill('0') << (int)pkt.queryID[i];
        ossNonce   << std::hex << std::setw(2) << std::setfill('0') << (int)pkt.nonce[i];
    }
    queryID = ossQueryID.str();
    nonce = ossNonce.str();
    return true;
}

static bool SendDiscoveryPacket(SOCKET sock, const sockaddr_in& targetAddr, const std::string& hostName) {
    mdns_packet pkt = BuildMDNSPacket(hostName, "query");
    encrypted_mdns_packet encryptedPacket{};
    if (!EncryptPacket(pkt, encryptedPacket)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "mDNS: unable to encrypt discovery packet.");
        return false;
    }

    const auto sent = sendto(sock, reinterpret_cast<const char*>(&encryptedPacket), sizeof(encryptedPacket), 0,
        reinterpret_cast<const sockaddr*>(&targetAddr), sizeof(targetAddr));
    const bool sentComplete = sent >= 0 && static_cast<size_t>(sent) == sizeof(encryptedPacket);
    g_logger.log(__FUNCTION__,
        sentComplete ? Logger::Level::DDebug : Logger::Level::Warning,
        "mDNS: sent discovery query for host '%s' (%zu bytes, result=%ld).",
        hostName.c_str(),
        sizeof(encryptedPacket),
        static_cast<long>(sent));
    return sentComplete;
}

static void WakeMDNSThread() {
    std::lock_guard<std::mutex> lock(g_mdnsSocketMutex);
    g_mdnsWakeEvent.Signal();
}

static void MDNSThreadProc(std::promise<bool> initPromise, MDNSCallback callback) {
    g_mdnsCallback = callback;

	in_addr multicastAddrn = {};
    if (1 != inet_pton(AF_INET, g_settings.multicastIp().c_str(), &multicastAddrn)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "mDNS: invalid multicast IP '%s'.", g_settings.multicastIp().c_str());
        initPromise.set_value(false);
		return;
    }
	in_addr listenerAddrn = {};
    if (1 != inet_pton(AF_INET, g_settings.listenerIp().c_str(), &listenerAddrn)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "mDNS: invalid listener IP '%s'.", g_settings.listenerIp().c_str());
        initPromise.set_value(false);
        return;
    }

    const std::string localHostName = GetLocalHostName();
    g_logger.log(__FUNCTION__,
        Logger::Level::Info,
        "mDNS: starting for host '%s' on %s:%hu, multicast %s:%hu.",
        localHostName.c_str(),
        g_settings.listenerIp().c_str(),
        static_cast<unsigned short>(g_settings.mdnsPort()),
        g_settings.multicastIp().c_str(),
        static_cast<unsigned short>(g_settings.mdnsPort()));

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "mDNS: socket creation failed.");
        initPromise.set_value(false);
        return;
    }
    if (!g_settings.getHostID(g_hostId)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "mDNS: host ID unavailable.");
        closesocket(sock);
        initPromise.set_value(false);
        return;
    }

    if (!g_mdnsWakeEvent.Initialize()) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "mDNS: wake socket creation failed.");
        closesocket(sock);
        initPromise.set_value(false);
        return;
    }

    int reuseAddr = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

    #ifdef __APPLE__
        int reusePort = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reusePort), sizeof(reusePort));
    #endif

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(static_cast<u_short>(g_settings.mdnsPort()));
    bindAddr.sin_addr.s_addr = listenerAddrn.s_addr;
    if (bind(sock, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "mDNS: bind failed.");
        g_mdnsWakeEvent.Close();
        closesocket(sock);
        initPromise.set_value(false);
        return;
    }

    ip_mreq group{};
    group.imr_multiaddr.s_addr = multicastAddrn.s_addr;
    group.imr_interface.s_addr = listenerAddrn.s_addr;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&group), sizeof(group)) == SOCKET_ERROR) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, "mDNS: joining multicast group failed; continuing to listen for direct traffic.");
    }

    sockaddr_in multicastAddr{};
    multicastAddr.sin_family = AF_INET;
    multicastAddr.sin_port = htons(static_cast<u_short>(g_settings.mdnsPort()));
    multicastAddr.sin_addr.s_addr = multicastAddrn.s_addr;

    {
        std::lock_guard<std::mutex> lock(g_mdnsSocketMutex);
        g_mdnsSock = sock;
    }
    g_mdnsSendImmediately = true;
    g_mdnsRunning = true;
    initPromise.set_value(true);
    g_logger.log(__FUNCTION__, Logger::Level::Info, "mDNS: thread initialized.");

    auto nextSendTime = std::chrono::steady_clock::now();
    std::array<char, 1024> recvBuffer{};

    while (g_mdnsRunning.load()) {
        const bool hasNetworkKey = HasNetworkKey();
        const auto now = std::chrono::steady_clock::now();
        if (g_mdnsSendImmediately.exchange(false)) {
            nextSendTime = now;
        }
        if (hasNetworkKey && now >= nextSendTime) {
            SendDiscoveryPacket(sock, multicastAddr, localHostName);
            nextSendTime = now + kBroadcastInterval;
        } else if (!hasNetworkKey) {
            nextSendTime = now;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        FD_SET(g_mdnsWakeEvent.Socket(), &readfds);

        timeval tv{};
        tv.tv_sec = 10;
        tv.tv_usec = 0;

        const SOCKET maxSock = (std::max)(sock, g_mdnsWakeEvent.Socket());
        const int ready = select(static_cast<int>(maxSock) + 1, &readfds, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR)
            break;

        if (!g_mdnsRunning.load())
            break;

        if (ready > 0 && FD_ISSET(g_mdnsWakeEvent.Socket(), &readfds)) {
            g_mdnsWakeEvent.Drain();
        }

        if (ready > 0 && FD_ISSET(sock, &readfds)) {
            sockaddr_in fromAddr{};
            socklen_t fromLen = sizeof(fromAddr);
            const auto bytesRead = recvfrom(sock, recvBuffer.data(), static_cast<int>(recvBuffer.size()), 0,
                reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
            if (bytesRead <= 0)
                continue;

            char senderIpForLog[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &fromAddr.sin_addr, senderIpForLog, sizeof(senderIpForLog));
            g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: received %ld bytes from %s.", static_cast<long>(bytesRead), senderIpForLog);

            if (!HasNetworkKey()) {
                g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: ignored packet because no network key is configured.");
                continue;
            }

            mdns_packet decryptedPacket;
            if (!DecryptPacket(recvBuffer.data(), static_cast<size_t>(bytesRead), decryptedPacket)) {
                g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: unable to decrypt packet.");
                continue;
            }

            std::string discoveredHost, verb, discoveredQueryID, discoveredNonce;
			unsigned short discoveredPort = 0;
            const unsigned char* rawQueryID = nullptr;
            HostId remoteHostId;
            if (!ParseDiscoveryPacket(decryptedPacket,
                discoveredHost,
                verb,
                discoveredQueryID,
                discoveredNonce,
                discoveredPort,
                &rawQueryID,
                remoteHostId))
            {
                g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: ignored decrypted packet that did not match the Clipp discovery schema.");
                continue;
            }

            if (verb == "query" && rawQueryID != nullptr) {
				// ignore our own queries
                if (remoteHostId == g_hostId) {
                    g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: ignored self query.");
                    continue;
                }
                mdns_packet responsePacket = BuildMDNSPacket(localHostName, "response", rawQueryID);
                encrypted_mdns_packet encryptedResponse{};
                if (EncryptPacket(responsePacket, encryptedResponse)) {
                    sendto(sock, reinterpret_cast<const char*>(&encryptedResponse), sizeof(encryptedResponse), 0,
                        reinterpret_cast<const sockaddr*>(&fromAddr), sizeof(fromAddr));
                    g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: sent discovery response to %s.", senderIpForLog);
                }
            }

            if (g_mdnsCallback) {
                char senderIp[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &fromAddr.sin_addr, senderIp, sizeof(senderIp));
				g_mdnsCallback(discoveredHost.c_str(), 
                    senderIp, 
                    discoveredQueryID.c_str(), 
                    discoveredNonce.c_str(), 
                    verb.c_str(), 
                    discoveredPort, 
                    remoteHostId);
            }
        }
    }

    if (sock != INVALID_SOCKET) {
        setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&group), sizeof(group));
        closesocket(sock);
    }

    {
        std::lock_guard<std::mutex> lock(g_mdnsSocketMutex);
        g_mdnsSock = INVALID_SOCKET;
        g_mdnsWakeEvent.Close();
    }
    g_mdnsRunning = false;
    g_logger.log(__FUNCTION__, Logger::Level::Info, "mDNS: thread exiting.");
}

bool StartMDNS(MDNSCallback callback) {
    std::promise<bool> initPromise;
    std::future<bool> initFuture = initPromise.get_future();
    g_mdnsThread = std::thread(MDNSThreadProc, std::move(initPromise), callback);
    if (!initFuture.get()) {
        if (g_mdnsThread.joinable())
            g_mdnsThread.join();
        return false;
    }
    return true;
}

void MDNSNotifyNetworkKeyChange() {
    g_mdnsSendImmediately = true;
    WakeMDNSThread();
}

void StopMDNS() {
    g_mdnsRunning = false;
    WakeMDNSThread();
    if (g_mdnsThread.joinable())
        g_mdnsThread.join();
}
