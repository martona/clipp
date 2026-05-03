#include "MDNSThread.h"
#include "Settings.h"

#include <thread>
#include <future>
#include <chrono>
#include <cstring>
#include <string>
#include <array>
#include <atomic>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

static std::thread g_mdnsThread;
static MDNSCallback g_mdnsCallback = nullptr;
static std::atomic<bool> g_mdnsRunning{ false };
static SOCKET g_mdnsSock = INVALID_SOCKET;

namespace {
constexpr const char* kProtocolSelector = "clipp";
constexpr int kProtocolVersion = 1;
constexpr auto kBroadcastInterval = std::chrono::minutes(1);

std::string GetLocalHostName() {
    char hostName[256] = {};
    if (gethostname(hostName, sizeof(hostName)) == 0)
        return hostName;
    return "unknown";
}

std::string BuildDiscoveryPacket(const std::string& hostName) {
    // Format: selector|version|hostname
    return std::string(kProtocolSelector) + "|" + std::to_string(kProtocolVersion) + "|" + hostName;
}

bool ParseDiscoveryPacket(const char* packet, int packetLen, std::string& hostName) {
    if (!packet || packetLen <= 0)
        return false;

    std::string payload(packet, packet + packetLen);
    const size_t firstSep = payload.find('|');
    const size_t secondSep = payload.find('|', firstSep == std::string::npos ? 0 : firstSep + 1);
    if (firstSep == std::string::npos || secondSep == std::string::npos)
        return false;

    const std::string selector = payload.substr(0, firstSep);
    const std::string versionStr = payload.substr(firstSep + 1, secondSep - firstSep - 1);
    const std::string parsedHost = payload.substr(secondSep + 1);

    if (selector != kProtocolSelector)
        return false;

    int version = 0;
    try {
        version = std::stoi(versionStr);
    }
    catch (...) {
        return false;
    }

    if (version != kProtocolVersion || parsedHost.empty())
        return false;

    hostName = parsedHost;
    return true;
}

bool SendDiscoveryPacket(SOCKET sock, const sockaddr_in& targetAddr, const std::string& hostName) {
    const std::string packet = BuildDiscoveryPacket(hostName);
    const int sent = sendto(sock, packet.c_str(), static_cast<int>(packet.size()), 0,
        reinterpret_cast<const sockaddr*>(&targetAddr), sizeof(targetAddr));
    return sent == static_cast<int>(packet.size());
}
} // namespace

static void MDNSThreadProc(std::promise<bool> initPromise, MDNSCallback callback) {
    g_mdnsCallback = callback;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        initPromise.set_value(false);
        return;
    }

    const int port = Settings::mdnsPort();
    const std::string multicastIp = Settings::multicastIp();
    const std::string localHostName = GetLocalHostName();

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        initPromise.set_value(false);
        return;
    }

    BOOL reuseAddr = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(static_cast<u_short>(port));
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(sock);
        WSACleanup();
        initPromise.set_value(false);
        return;
    }

    ip_mreq group{};
    group.imr_multiaddr.s_addr = inet_addr(multicastIp.c_str());
    group.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&group), sizeof(group));

    sockaddr_in multicastAddr{};
    multicastAddr.sin_family = AF_INET;
    multicastAddr.sin_port = htons(static_cast<u_short>(port));
    multicastAddr.sin_addr.s_addr = inet_addr(multicastIp.c_str());

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
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        const int ready = select(0, &readfds, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR)
            break;

        if (ready > 0 && FD_ISSET(sock, &readfds)) {
            sockaddr_in fromAddr{};
            int fromLen = sizeof(fromAddr);
            const int bytesRead = recvfrom(sock, recvBuffer.data(), static_cast<int>(recvBuffer.size()), 0,
                reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
            if (bytesRead <= 0)
                continue;

            std::string discoveredHost;
            if (ParseDiscoveryPacket(recvBuffer.data(), bytesRead, discoveredHost) && g_mdnsCallback)
                g_mdnsCallback(discoveredHost.c_str());
        }
    }

    if (sock != INVALID_SOCKET) {
        setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&group), sizeof(group));
        closesocket(sock);
    }

    g_mdnsSock = INVALID_SOCKET;
    g_mdnsRunning = false;
    WSACleanup();
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
