#include "MDNSProtocol.h"

#include "KeyManager.h"
#include "Settings.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

extern Settings g_settings;
extern KeyManager g_keyManager;

namespace {
constexpr const char* kProtocolSelector = "clipp";
constexpr int kProtocolVersion = 2;

bool GetMDNSKey(KeyManager::NetworkKey& key) {
    std::string errorMessage;
    return g_keyManager.GetKey(KeyManager::KeyRole::MDNS, key, &errorMessage);
}

std::string HexString(const unsigned char* bytes, std::size_t count) {
    std::ostringstream output;
    for (std::size_t idx = 0; idx < count; ++idx) {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[idx]);
    }
    return output.str();
}

}

namespace MDNSProtocol {

Packet BuildPacket(const std::string& hostName,
                   const HostId& hostID,
                   unsigned short port,
                   const char* verb,
                   const unsigned char* queryID) {
    Packet packet;
    packet.version = htons(kProtocolVersion);
    randombytes_buf(packet.nonce, sizeof(packet.nonce));
    strncpys(packet.selector, kProtocolSelector);
    strncpys(packet.hostName, hostName.c_str());
    strncpys(packet.verb, verb != nullptr ? verb : "");
    packet.port = htons(port);
    std::memcpy(packet.hostID, hostID.data().data(), sizeof(packet.hostID));
    if (queryID != nullptr) {
        std::memcpy(packet.queryID, queryID, sizeof(packet.queryID));
    } else {
        randombytes_buf(packet.queryID, sizeof(packet.queryID));
    }
    return packet;
}

bool EncryptPacket(const Packet& packet, EncryptedPacket& encryptedPacket) {
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

bool DecryptPacket(const char* packet, size_t packetLen, Packet& decryptedPacket) {
    if (packet == nullptr || packetLen != sizeof(EncryptedPacket)) {
        return false;
    }

    KeyManager::NetworkKey mdnsKey{};
    if (!GetMDNSKey(mdnsKey)) {
        return false;
    }

    const auto* encryptedPacket = reinterpret_cast<const EncryptedPacket*>(packet);
    return crypto_secretbox_open_easy(
        reinterpret_cast<unsigned char*>(&decryptedPacket),
        encryptedPacket->ciphertext,
        sizeof(encryptedPacket->ciphertext),
        encryptedPacket->nonce,
        mdnsKey.data()) == 0;
}

bool ParsePacket(Packet& packet, ParsedPacket& parsedPacket) {
    if (std::strncmp(packet.selector, kProtocolSelector, cntof(packet.selector)) != 0) {
        return false;
    }
    if (packet.version != htons(kProtocolVersion)) {
        return false;
    }
    if (packet.hostName[0] == 0 || packet.verb[0] == 0) {
        return false;
    }

    packet.hostName[cntof(packet.hostName) - 1] = 0;
    packet.verb[cntof(packet.verb) - 1] = 0;
    parsedPacket.hostName = packet.hostName;
    parsedPacket.verb = packet.verb;
    parsedPacket.remoteHostID = HostId(packet.hostID);
    parsedPacket.port = ntohs(packet.port);
    std::memcpy(parsedPacket.queryID.data(), packet.queryID, parsedPacket.queryID.size());
    parsedPacket.queryIDHex = HexString(packet.queryID, sizeof(packet.queryID));
    parsedPacket.nonceHex = HexString(packet.nonce, sizeof(packet.nonce));
    return true;
}

std::string GetLocalHostName(const char* fallback) {
    char hostName[256] = {};
    if (gethostname(hostName, sizeof(hostName)) == 0) {
        return hostName;
    }
    return fallback != nullptr ? fallback : "unknown";
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

bool ProbePeersOnce(std::chrono::milliseconds wait, std::vector<DiscoveredPeer>& peers) {
    peers.clear();

    HostId localHostID;
    if (!g_settings.getHostID(localHostID)) {
        return false;
    }

    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        return false;
    }

    int reuseAddr = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = 0;
    if (inet_pton(AF_INET, g_settings.listenerIp().c_str(), &bindAddr.sin_addr) != 1
        || bind(socket, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(socket);
        return false;
    }

    sockaddr_in multicastAddr{};
    multicastAddr.sin_family = AF_INET;
    multicastAddr.sin_port = htons(static_cast<u_short>(g_settings.mdnsPort()));
    if (inet_pton(AF_INET, g_settings.multicastIp().c_str(), &multicastAddr.sin_addr) != 1) {
        closesocket(socket);
        return false;
    }

    std::array<unsigned char, QueryIDSize> queryID{};
    randombytes_buf(queryID.data(), queryID.size());

    Packet packet = BuildPacket(GetLocalHostName("iPhone"),
                                localHostID,
                                static_cast<unsigned short>(g_settings.tcpPort()),
                                "query",
                                queryID.data());
    EncryptedPacket encryptedPacket{};
    if (!EncryptPacket(packet, encryptedPacket)) {
        closesocket(socket);
        return false;
    }

    const auto sent = sendto(socket,
                             reinterpret_cast<const char*>(&encryptedPacket),
                             sizeof(encryptedPacket),
                             0,
                             reinterpret_cast<const sockaddr*>(&multicastAddr),
                             sizeof(multicastAddr));
    if (sent < 0 || static_cast<size_t>(sent) != sizeof(encryptedPacket)) {
        closesocket(socket);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + wait;
    std::array<char, sizeof(EncryptedPacket)> recvBuffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        const auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(remaining);

        timeval timeout{};
        timeout.tv_sec = static_cast<long>(remainingUs.count() / 1000000);
        timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(remainingUs.count() % 1000000);

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

        Packet decryptedPacket;
        if (!DecryptPacket(recvBuffer.data(), static_cast<size_t>(bytesRead), decryptedPacket)) {
            continue;
        }

        ParsedPacket parsedPacket;
        if (!ParsePacket(decryptedPacket, parsedPacket)
            || parsedPacket.verb != "response"
            || std::memcmp(parsedPacket.queryID.data(), queryID.data(), queryID.size()) != 0
            || parsedPacket.remoteHostID == localHostID
            || parsedPacket.port == 0) {
            continue;
        }

        char senderIp[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &fromAddr.sin_addr, senderIp, sizeof(senderIp)) == nullptr) {
            continue;
        }

        AddUniquePeer(peers, DiscoveredPeer{
            parsedPacket.hostName,
            senderIp,
            parsedPacket.remoteHostID,
            parsedPacket.port,
        });
    }

    closesocket(socket);
    return true;
}

}
