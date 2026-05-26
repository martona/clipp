#pragma once

#include "HostId.h"
#include "OsType.h"
#include "platform.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

namespace MDNSProtocol {

constexpr uint32_t kPacketMagic = 0x434C4950; // 'CLIP'
constexpr uint16_t kPacketVersion = 1;

// Plaintext payload that goes inside the encrypted TXT blob.
// 100 bytes; secretbox-wrapped to 140 (24 nonce + 16 MAC + 100 cipher); base64 to 188 chars.
// Comfortably fits in a single DNS TXT key=value sub-string.
#pragma pack(push, 1)
struct PacketV1 {
    uint32_t magic;             // host-order kPacketMagic; sanity-check after decrypt
    uint16_t version;           // network-order kPacketVersion
    uint16_t flags;             // network-order; reserved (0)
    uint16_t osType;            // network-order OsType
    uint16_t pad;               // alignment
    uint8_t  hostId[HostId::kSize];
    uint8_t  caps[8];           // capability bits (currently unused; ship populated as 0)
    char     deviceName[64];    // UTF-8, zero-padded
};
#pragma pack(pop)

static_assert(sizeof(PacketV1) == 100, "PacketV1 must be 100 bytes for wire compat");

// Builds a freshly-populated packet describing the local host.
PacketV1 BuildLocalPacket(const std::string& deviceName, const HostId& hostId, OsType osType);

// TXT record key/value pairs.
//   "v" -> "1"                  plaintext protocol version (lets peers skip unknown versions cheaply)
//   "d" -> base64(nonce||MAC||cipher)   encrypted PacketV1
constexpr const char* kTxtKeyVersion = "v";
constexpr const char* kTxtKeyData = "d";

// Encode: encrypts packet under the discovery network key and writes v/d into outTxt.
// Returns false if the network key is unavailable or encryption fails.
bool EncodeTxt(const PacketV1& packet, std::map<std::string, std::string>& outTxt);

// Decode: looks up v/d in txt, validates v == 1, base64-decodes d, decrypts under the
// network key, validates magic + version inside the plaintext. Returns false on any failure.
bool DecodeTxt(const std::map<std::string, std::string>& txt, PacketV1& outPacket);

// Returns the local device name, suitable for the deviceName field.
std::string GetLocalDeviceName();

} // namespace MDNSProtocol
