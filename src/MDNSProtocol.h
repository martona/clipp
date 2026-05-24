#pragma once

#include "HostId.h"
#include "platform.h"

#include <sodium.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace MDNSProtocol {

static constexpr std::size_t QueryIDSize = 32;

struct Packet {
    Packet() {
        std::memset(this, 0, sizeof(*this));
    }

    char selector[16];
    u_short version;
    char hostName[256];
    unsigned char hostID[HostId::kSize];
    u_short port;
    char verb[16];
    unsigned char queryID[QueryIDSize];
    unsigned char nonce[32];
};

struct EncryptedPacket {
    unsigned char nonce[crypto_secretbox_NONCEBYTES];
    unsigned char ciphertext[sizeof(Packet) + crypto_secretbox_MACBYTES];
};

struct ParsedPacket {
    std::string hostName;
    std::string verb;
    std::string queryIDHex;
    std::string nonceHex;
    std::array<unsigned char, QueryIDSize> queryID{};
    HostId remoteHostID;
    unsigned short port = 0;
};

struct DiscoveredPeer {
    std::string hostName;
    std::string ip;
    HostId hostID;
    unsigned short port = 0;
};

Packet BuildPacket(const std::string& hostName,
                   const HostId& hostID,
                   unsigned short port,
                   const char* verb,
                   const unsigned char* queryID = nullptr);
bool EncryptPacket(const Packet& packet, EncryptedPacket& encryptedPacket);
bool DecryptPacket(const char* packet, size_t packetLen, Packet& decryptedPacket);
bool ParsePacket(Packet& packet, ParsedPacket& parsedPacket);
std::string GetLocalHostName(const char* fallback = "unknown");
bool AddUniquePeer(std::vector<DiscoveredPeer>& peers, DiscoveredPeer peer);
bool ProbePeersOnce(std::chrono::milliseconds wait, std::vector<DiscoveredPeer>& peers);

}
