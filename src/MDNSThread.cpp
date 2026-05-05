#include "platform.h"
#include "MDNSThread.h"
#include "Settings.h"
#include "KeyManager.h"
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

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Crypt32.lib")

static std::thread g_mdnsThread;
static MDNSCallback g_mdnsCallback = nullptr;
static std::atomic<bool> g_mdnsRunning{ false };
static SOCKET g_mdnsSock = INVALID_SOCKET;
static std::array<unsigned char, 32> g_lastSentQueryID{};
static std::array<unsigned char, 32> g_hostID{};

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

static bool GetNetworkKey(std::array<unsigned char, KeyManager::NetworkKeySize>& networkKey) {
    std::string errorMessage;
    return g_keyManager.GetNetworkKey(networkKey, &errorMessage);
}

static bool EncryptPacket(const mdns_packet& packet, encrypted_mdns_packet& encryptedPacket) {
    std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
    if (!GetNetworkKey(networkKey))
        return false;

    randombytes_buf(encryptedPacket.nonce, sizeof(encryptedPacket.nonce));
    return crypto_secretbox_easy(
        encryptedPacket.ciphertext,
        reinterpret_cast<const unsigned char*>(&packet),
        sizeof(packet),
        encryptedPacket.nonce,
        networkKey.data()) == 0;
}

static bool DecryptPacket(const char* packet, size_t packetLen, mdns_packet& decryptedPacket) {
    if (!packet || packetLen != sizeof(encrypted_mdns_packet))
        return false;

    const encrypted_mdns_packet* encryptedPacket = reinterpret_cast<const encrypted_mdns_packet*>(packet);
    std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
    if (!GetNetworkKey(networkKey))
        return false;

    return crypto_secretbox_open_easy(
        reinterpret_cast<unsigned char*>(&decryptedPacket),
        encryptedPacket->ciphertext,
        sizeof(encryptedPacket->ciphertext),
        encryptedPacket->nonce,
        networkKey.data()) == 0;
}

static std::string GetLocalHostName() {
    char hostName[256] = {};
	if (gethostname(hostName, sizeof(hostName)) == 0) {
        return hostName;
    }
    return "unknown";
}

static mdns_packet BuildDiscoveryPacket(const std::string& hostName) {
	mdns_packet packet;
	packet.version = htons(kProtocolVersion);
	randombytes_buf(packet.queryID, sizeof(packet.queryID));
	randombytes_buf(packet.nonce, sizeof(packet.nonce));
	strncpys(packet.selector, kProtocolSelector, cntof(packet.selector));
	strncpys(packet.hostName, hostName.c_str(), cntof(packet.hostName));
	strncpys(packet.verb, "query", cntof(packet.verb));
    std::memcpy(packet.hostID, g_hostID.data(), sizeof(packet.hostID));
	std::memcpy(g_lastSentQueryID.data(), packet.queryID, sizeof(packet.queryID));
    return packet;
}

static mdns_packet BuildResponsePacket(const std::string& hostName, const unsigned char* queryID) {
    mdns_packet packet;
    packet.version = htons(kProtocolVersion);
    randombytes_buf(packet.nonce, sizeof(packet.nonce));
    strncpys(packet.selector, kProtocolSelector, cntof(packet.selector));
    strncpys(packet.hostName, hostName.c_str(), cntof(packet.hostName));
    strncpys(packet.verb, "response", cntof(packet.verb));
	packet.port = htons(static_cast<u_short>(g_settings.tcpPort()));
    std::memcpy(packet.hostID, g_hostID.data(), sizeof(packet.hostID));
    std::memcpy(packet.queryID, queryID, sizeof(packet.queryID));
    return packet;
}

static bool ParseDiscoveryPacket(mdns_packet& pkt, 
                                std::string& hostName, 
                                std::string& hostID, 
                                std::string& verb, 
                                std::string& queryID, 
                                std::string& nonce, 
                                unsigned short& hostPort, 
                                const unsigned char** rawQueryID, 
                                const unsigned char** rawHostID) 
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
	// Force null-termination of hostName and verb
	pkt.hostName[cntof(pkt.hostName) - 1] = 0;
	pkt.verb[cntof(pkt.verb) - 1] = 0;
    hostName = pkt.hostName;
    verb = pkt.verb;
    if (rawQueryID)
        *rawQueryID = pkt.queryID;
    if (rawHostID)
		*rawHostID = pkt.hostID;

    if (verb == "response" && std::memcmp(pkt.queryID, g_lastSentQueryID.data(), sizeof(pkt.queryID)) != 0)
        return false;

	hostPort = ntohs(pkt.port);

    // Convert queryID and nonce to hex wstring
    std::ostringstream ossQueryID, ossNonce, ossHostID;
    for (int i = 0; i < 32; ++i) {
        ossQueryID << std::hex << std::setw(2) << std::setfill('0') << (int)pkt.queryID[i];
        ossNonce   << std::hex << std::setw(2) << std::setfill('0') << (int)pkt.nonce[i];
        ossHostID  << std::hex << std::setw(2) << std::setfill('0') << (int)pkt.hostID[i];
    }
    hostID = ossHostID.str();
    queryID = ossQueryID.str();
    nonce = ossNonce.str();
    return true;
}

static bool SendDiscoveryPacket(SOCKET sock, const sockaddr_in& targetAddr, const std::string& hostName) {
    mdns_packet pkt = BuildDiscoveryPacket(hostName);
    encrypted_mdns_packet encryptedPacket{};
    if (!EncryptPacket(pkt, encryptedPacket))
        return false;

    size_t sent = sendto(sock, reinterpret_cast<const char*>(&encryptedPacket), sizeof(encryptedPacket), 0,
        reinterpret_cast<const sockaddr*>(&targetAddr), sizeof(targetAddr));
    return sent == sizeof(encryptedPacket);
}

static void MDNSThreadProc(std::promise<bool> initPromise, MDNSCallback callback) {
    g_mdnsCallback = callback;

	in_addr multicastAddrn = {};
    if (1 != inet_pton(AF_INET, g_settings.multicastIp().c_str(), &multicastAddrn)) {
        initPromise.set_value(false);
		return;
    }
	in_addr listenerAddrn = {};
    if (1 != inet_pton(AF_INET, g_settings.listenerIp().c_str(), &listenerAddrn)) {
        initPromise.set_value(false);
        return;
    }

    const std::string localHostName = GetLocalHostName();

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        initPromise.set_value(false);
        return;
    }
    if (!g_settings.getHostID(g_hostID)) {
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
        closesocket(sock);
        initPromise.set_value(false);
        return;
    }

    ip_mreq group{};
    group.imr_multiaddr.s_addr = multicastAddrn.s_addr;
    group.imr_interface.s_addr = listenerAddrn.s_addr;
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&group), sizeof(group));

    sockaddr_in multicastAddr{};
    multicastAddr.sin_family = AF_INET;
    multicastAddr.sin_port = htons(static_cast<u_short>(g_settings.mdnsPort()));
    multicastAddr.sin_addr.s_addr = multicastAddrn.s_addr;

    g_mdnsSock = sock;
    g_mdnsRunning = true;
    initPromise.set_value(true);

    auto nextSendTime = std::chrono::steady_clock::now();
    std::array<char, 1024> recvBuffer{};

    while (g_mdnsRunning.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSendTime) {
            SendDiscoveryPacket(sock, multicastAddr, localHostName);
            nextSendTime = now + kBroadcastInterval;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        timeval tv{};
        tv.tv_sec = 10;
        tv.tv_usec = 0;

        const int ready = select(static_cast<int>(sock) + 1, &readfds, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR)
            break;

        if (ready > 0 && FD_ISSET(sock, &readfds)) {
            sockaddr_in fromAddr{};
            socklen_t fromLen = sizeof(fromAddr);
            const size_t bytesRead = recvfrom(sock, recvBuffer.data(), static_cast<int>(recvBuffer.size()), 0,
                reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
            if (bytesRead == 0)
                continue;

            mdns_packet decryptedPacket;
            if (!DecryptPacket(recvBuffer.data(), bytesRead, decryptedPacket))
                continue;

            std::string discoveredHost, discoveredHostID, verb, discoveredQueryID, discoveredNonce;
			unsigned short discoveredPort = 0;
            const unsigned char* rawQueryID = nullptr;
            const unsigned char* rawHostID = nullptr;
            if (!ParseDiscoveryPacket(decryptedPacket,
                discoveredHost,
                discoveredHostID,
                verb,
                discoveredQueryID,
                discoveredNonce,
                discoveredPort,
                &rawQueryID,
                &rawHostID))
            {
                continue;
            }

            if (verb == "query" && rawQueryID != nullptr) {
                SOCKET responseSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (responseSock != INVALID_SOCKET) {
                    mdns_packet responsePacket = BuildResponsePacket(localHostName, rawQueryID);
                    encrypted_mdns_packet encryptedResponse{};
                    if (EncryptPacket(responsePacket, encryptedResponse)) {
                        sendto(responseSock, reinterpret_cast<const char*>(&encryptedResponse), sizeof(encryptedResponse), 0,
                            reinterpret_cast<const sockaddr*>(&fromAddr), sizeof(fromAddr));
                    }
                    closesocket(responseSock);
                }
            }

            if (g_mdnsCallback && rawHostID) {
                char senderIp[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &fromAddr.sin_addr, senderIp, sizeof(senderIp));
				g_mdnsCallback(discoveredHost.c_str(), 
                    discoveredHostID.c_str(), 
                    senderIp, 
                    discoveredQueryID.c_str(), 
                    discoveredNonce.c_str(), 
                    verb.c_str(), 
                    discoveredPort, 
                    rawHostID);
            }
        }
    }

    if (sock != INVALID_SOCKET) {
        setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&group), sizeof(group));
        closesocket(sock);
    }

    g_mdnsSock = INVALID_SOCKET;
    g_mdnsRunning = false;
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

void StopMDNS() {
    g_mdnsRunning = false;
    if (g_mdnsSock != INVALID_SOCKET) {
        closesocket(g_mdnsSock);
        g_mdnsSock = INVALID_SOCKET;
    }
    if (g_mdnsThread.joinable())
        g_mdnsThread.join();
}
