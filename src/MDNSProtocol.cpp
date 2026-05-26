#include "MDNSProtocol.h"

#include "KeyManager.h"
#include "LocalPeerName.h"
#include "Logger.h"

#include <sodium.h>

#include <cstring>
#include <vector>

extern KeyManager g_keyManager;

namespace MDNSProtocol {

namespace {

bool GetMDNSKey(KeyManager::NetworkKey& key) {
    std::string errorMessage;
    return g_keyManager.GetKey(KeyManager::KeyRole::MDNS, key, &errorMessage);
}

} // namespace

PacketV1 BuildLocalPacket(const std::string& deviceName, const HostId& hostId, OsType osType) {
    PacketV1 packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.magic = kPacketMagic;
    packet.version = htons(kPacketVersion);
    packet.flags = 0;
    packet.osType = htons(static_cast<uint16_t>(osType));
    std::memcpy(packet.hostId, hostId.data().data(), sizeof(packet.hostId));
    strncpys(packet.deviceName, deviceName.c_str());
    return packet;
}

bool EncodeTxt(const PacketV1& packet, std::map<std::string, std::string>& outTxt) {
    KeyManager::NetworkKey mdnsKey{};
    if (!GetMDNSKey(mdnsKey)) {
        return false;
    }

    unsigned char nonce[crypto_secretbox_NONCEBYTES];
    randombytes_buf(nonce, sizeof(nonce));

    std::vector<unsigned char> wire(sizeof(nonce) + crypto_secretbox_MACBYTES + sizeof(PacketV1));
    std::memcpy(wire.data(), nonce, sizeof(nonce));

    if (crypto_secretbox_easy(wire.data() + sizeof(nonce),
                              reinterpret_cast<const unsigned char*>(&packet),
                              sizeof(packet),
                              nonce,
                              mdnsKey.data()) != 0) {
        return false;
    }

    // No-padding base64: Windows DNS-SD strips trailing '=' from TXT values (the '=' is the
    // key/value separator on the wire), so encoding with padding causes the value to come
    // back shorter than expected on decode. Skip padding entirely on both ends.
    const size_t b64Len = sodium_base64_encoded_len(wire.size(), sodium_base64_VARIANT_ORIGINAL_NO_PADDING);
    std::string b64(b64Len, '\0');
    sodium_bin2base64(b64.data(), b64.size(),
                      wire.data(), wire.size(),
                      sodium_base64_VARIANT_ORIGINAL_NO_PADDING);
    if (!b64.empty() && b64.back() == '\0') {
        b64.pop_back();
    }

    outTxt.clear();
    outTxt[kTxtKeyVersion] = "1";
    outTxt[kTxtKeyData] = std::move(b64);
    return true;
}

bool DecodeTxt(const std::map<std::string, std::string>& txt, PacketV1& outPacket) {
    const auto versionIt = txt.find(kTxtKeyVersion);
    if (versionIt == txt.end()) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "DecodeTxt: 'v' key missing.");
        return false;
    }
    if (versionIt->second != "1") {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "DecodeTxt: 'v' is '%s', expected '1'.", versionIt->second.c_str());
        return false;
    }
    const auto dataIt = txt.find(kTxtKeyData);
    if (dataIt == txt.end()) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "DecodeTxt: 'd' key missing.");
        return false;
    }
    const std::string& b64 = dataIt->second;

    constexpr size_t wireLen = crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + sizeof(PacketV1);
    std::vector<unsigned char> wire(wireLen);
    size_t decodedLen = 0;
    int rc = sodium_base642bin(wire.data(), wire.size(),
                               b64.data(), b64.size(),
                               nullptr,
                               &decodedLen,
                               nullptr,
                               sodium_base64_VARIANT_ORIGINAL_NO_PADDING);
    if (rc != 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "DecodeTxt: base642bin failed (rc=%d) on b64 of len %zu.", rc, b64.size());
        return false;
    }
    if (decodedLen != wireLen) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "DecodeTxt: base64 decoded to %zu bytes, expected %zu.", decodedLen, wireLen);
        return false;
    }

    KeyManager::NetworkKey mdnsKey{};
    if (!GetMDNSKey(mdnsKey)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "DecodeTxt: GetMDNSKey failed (network key not configured?).");
        return false;
    }

    const unsigned char* nonce = wire.data();
    const unsigned char* cipher = wire.data() + crypto_secretbox_NONCEBYTES;
    const size_t cipherLen = crypto_secretbox_MACBYTES + sizeof(PacketV1);

    if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(&outPacket),
                                   cipher, cipherLen,
                                   nonce,
                                   mdnsKey.data()) != 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "DecodeTxt: secretbox_open_easy failed (wrong key, or MAC mismatch).");
        return false;
    }

    if (outPacket.magic != kPacketMagic) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "DecodeTxt: bad magic after decrypt (got 0x%08x, expected 0x%08x).",
            static_cast<unsigned>(outPacket.magic), static_cast<unsigned>(kPacketMagic));
        return false;
    }
    if (ntohs(outPacket.version) != kPacketVersion) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, "DecodeTxt: bad version after decrypt (got %u).", ntohs(outPacket.version));
        return false;
    }
    // Guard against any nasties in the embedded string.
    outPacket.deviceName[sizeof(outPacket.deviceName) - 1] = '\0';
    return true;
}

std::string GetLocalDeviceName() {
    return clipp::GetLocalPeerDisplayName("unknown", sizeof(PacketV1::deviceName));
}

} // namespace MDNSProtocol
