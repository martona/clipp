#include "platform.h"
#include "MDNSThread.h"
#include "Settings.h"
#include "KeyManager.h"
#include "Logger.h"
#include "MDNSProtocol.h"
#include <thread>
#include <future>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <string>
#include <array>
#include <atomic>
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
static std::atomic<bool> g_mdnsReloadHostID{ false };
static std::atomic<bool> g_hostIDCollisionWarning{ false };
static constexpr std::size_t kRecentOriginatedQueryIDCount = 8;
static std::array<std::array<unsigned char, MDNSProtocol::QueryIDSize>, kRecentOriginatedQueryIDCount> g_recentOriginatedQueryIDs{};
static std::size_t g_recentOriginatedQueryIDNext = 0;
static std::size_t g_recentOriginatedQueryIDUsed = 0;
static HostId g_hostId;

namespace {
    constexpr auto kBroadcastInterval = std::chrono::minutes(1);

    const char* SocketErrorText(int error) {
#ifdef _WIN32
        (void)error;
        return "";
#else
        const char* message = std::strerror(error);
        return message != nullptr ? message : "";
#endif
    }
}

static void RecordOriginatedQueryID(const unsigned char* queryID) {
    if (queryID == nullptr) {
        return;
    }

    std::memcpy(g_recentOriginatedQueryIDs[g_recentOriginatedQueryIDNext].data(), queryID, MDNSProtocol::QueryIDSize);
    g_recentOriginatedQueryIDNext = (g_recentOriginatedQueryIDNext + 1) % g_recentOriginatedQueryIDs.size();
    if (g_recentOriginatedQueryIDUsed < g_recentOriginatedQueryIDs.size()) {
        ++g_recentOriginatedQueryIDUsed;
    }
}

static bool IsRecentOriginatedQueryID(const unsigned char* queryID) {
    if (queryID == nullptr) {
        return false;
    }

    for (std::size_t i = 0; i < g_recentOriginatedQueryIDUsed; ++i) {
        if (std::memcmp(g_recentOriginatedQueryIDs[i].data(), queryID, MDNSProtocol::QueryIDSize) == 0) {
            return true;
        }
    }
    return false;
}

static void ClearRecentOriginatedQueryIDs() {
    for (auto& queryID : g_recentOriginatedQueryIDs) {
        queryID.fill(0);
    }
    g_recentOriginatedQueryIDNext = 0;
    g_recentOriginatedQueryIDUsed = 0;
}

static bool HasNetworkKey() {
    std::array<unsigned char, KeyManager::NetworkKeySize> mdnsKey{};
    std::string errorMessage;
    return g_keyManager.GetKey(KeyManager::KeyRole::MDNS, mdnsKey, &errorMessage);
}

static MDNSProtocol::Packet BuildMDNSPacket(const std::string& hostName, const char* verb, const unsigned char* queryID = nullptr) {
    MDNSProtocol::Packet packet = MDNSProtocol::BuildPacket(hostName,
        g_hostId,
        static_cast<unsigned short>(g_settings.tcpPort()),
        verb,
        queryID);
    if (queryID == nullptr) {
        RecordOriginatedQueryID(packet.queryID);
    }
    return packet;
}

static bool SendDiscoveryPacket(SOCKET sock, const sockaddr_in& targetAddr, const std::string& hostName) {
    MDNSProtocol::Packet pkt = BuildMDNSPacket(hostName, "query");
    MDNSProtocol::EncryptedPacket encryptedPacket{};
    if (!MDNSProtocol::EncryptPacket(pkt, encryptedPacket)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "mDNS: unable to encrypt discovery packet.");
        return false;
    }

    const auto sent = sendto(sock, reinterpret_cast<const char*>(&encryptedPacket), sizeof(encryptedPacket), 0,
        reinterpret_cast<const sockaddr*>(&targetAddr), sizeof(targetAddr));
    const bool sentComplete = sent >= 0 && static_cast<size_t>(sent) == sizeof(encryptedPacket);
    if (sentComplete) {
        g_logger.log(__FUNCTION__,
            Logger::Level::DDebug,
            "mDNS: sent discovery query for host '%s' (%zu bytes, result=%ld).",
            hostName.c_str(),
            sizeof(encryptedPacket),
            static_cast<long>(sent));
    } else {
        const int error = LastSocketError();
        g_logger.log(__FUNCTION__,
            Logger::Level::Warning,
            "mDNS: failed to send discovery query for host '%s' (%zu bytes, result=%ld, errno=%d %s).",
            hostName.c_str(),
            sizeof(encryptedPacket),
            static_cast<long>(sent),
            error,
            SocketErrorText(error));
    }
    return sentComplete;
}

static void WakeMDNSThread() {
    std::lock_guard<std::mutex> lock(g_mdnsSocketMutex);
    g_mdnsWakeEvent.Signal();
}

static void ReportPossibleHostIDCollision(const std::string& verb, const char* senderIpForLog) {
    const bool firstReport = !g_hostIDCollisionWarning.exchange(true);
    g_logger.log(__FUNCTION__,
        firstReport ? Logger::Level::Warning : Logger::Level::Debug,
        "mDNS: possible host ID collision detected from %s (%s packet used this host ID). If this device was restored from backup or cloned, reset Host ID in Settings.",
        senderIpForLog != nullptr ? senderIpForLog : "<unknown>",
        verb.c_str());
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

    const std::string localHostName = MDNSProtocol::GetLocalHostName();
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
    ClearRecentOriginatedQueryIDs();

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
        const int error = LastSocketError();
        g_logger.log(__FUNCTION__,
            Logger::Level::Warning,
            "mDNS: joining multicast group failed (errno=%d %s); continuing to listen for direct traffic.",
            error,
            SocketErrorText(error));
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
    std::array<char, sizeof(MDNSProtocol::EncryptedPacket)> recvBuffer{};

    while (g_mdnsRunning.load()) {
        if (g_mdnsReloadHostID.exchange(false)) {
            HostId refreshedHostId;
            if (g_settings.getHostID(refreshedHostId)) {
                g_hostId = refreshedHostId;
                ClearRecentOriginatedQueryIDs();
                g_logger.log(__FUNCTION__, Logger::Level::Info, "mDNS: local host ID reloaded.");
            } else {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, "mDNS: unable to reload local host ID.");
            }
        }

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

            MDNSProtocol::Packet decryptedPacket;
            if (!MDNSProtocol::DecryptPacket(recvBuffer.data(), static_cast<size_t>(bytesRead), decryptedPacket)) {
                g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: unable to decrypt packet.");
                continue;
            }

            MDNSProtocol::ParsedPacket parsedPacket;
            if (!MDNSProtocol::ParsePacket(decryptedPacket, parsedPacket)) {
                g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: ignored decrypted packet that did not match the Clipp discovery schema.");
                continue;
            }
            if (parsedPacket.verb == "response" && !IsRecentOriginatedQueryID(parsedPacket.queryID.data())) {
                g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: ignored response for an unknown query.");
                continue;
            }

            if (parsedPacket.remoteHostID == g_hostId) {
                if (parsedPacket.verb == "query" && IsRecentOriginatedQueryID(parsedPacket.queryID.data())) {
                    g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: ignored self query.");
                    continue;
                }

                ReportPossibleHostIDCollision(parsedPacket.verb, senderIpForLog);
                continue;
            }

            if (parsedPacket.verb == "query") {
                MDNSProtocol::Packet responsePacket = BuildMDNSPacket(localHostName, "response", parsedPacket.queryID.data());
                MDNSProtocol::EncryptedPacket encryptedResponse{};
                if (MDNSProtocol::EncryptPacket(responsePacket, encryptedResponse)) {
                    const auto sent = sendto(sock, reinterpret_cast<const char*>(&encryptedResponse), sizeof(encryptedResponse), 0,
                        reinterpret_cast<const sockaddr*>(&fromAddr), sizeof(fromAddr));
                    if (sent >= 0 && static_cast<size_t>(sent) == sizeof(encryptedResponse)) {
                        g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS: sent discovery response to %s.", senderIpForLog);
                    } else {
                        const int error = LastSocketError();
                        g_logger.log(__FUNCTION__,
                            Logger::Level::Warning,
                            "mDNS: failed to send discovery response to %s (result=%ld, errno=%d %s).",
                            senderIpForLog,
                            static_cast<long>(sent),
                            error,
                            SocketErrorText(error));
                    }
                }
            }

            if (g_mdnsCallback) {
                char senderIp[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &fromAddr.sin_addr, senderIp, sizeof(senderIp));
				g_mdnsCallback(parsedPacket.hostName.c_str(),
                    senderIp,
                    parsedPacket.queryIDHex.c_str(),
                    parsedPacket.nonceHex.c_str(),
                    parsedPacket.verb.c_str(),
                    parsedPacket.port,
                    parsedPacket.remoteHostID);
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

void MDNSNotifyHostIDChange() {
    g_hostIDCollisionWarning = false;
    g_mdnsReloadHostID = true;
    g_mdnsSendImmediately = true;
    WakeMDNSThread();
}

bool MDNSHasHostIDCollisionWarning() {
    return g_hostIDCollisionWarning.load();
}

void MDNSClearHostIDCollisionWarning() {
    g_hostIDCollisionWarning = false;
}

void StopMDNS() {
    g_mdnsRunning = false;
    WakeMDNSThread();
    if (g_mdnsThread.joinable())
        g_mdnsThread.join();
}
