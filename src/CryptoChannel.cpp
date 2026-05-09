#include "CryptoChannel.h"

#include <vector>
#include <cstring>

#include "platform.h"
#include "Settings.h"
#include "KeyManager.h"
#include "utils.h"
#include "utils_socket.h"

namespace {
#pragma pack(push, 1)
    struct HandshakePlaintext {
        unsigned char ephemeralPk[crypto_kx_PUBLICKEYBYTES];
        unsigned char hostId[CryptoChannel::HostIdSize];
        char hostNameUTF8[CryptoChannel::HOSTNAME_MAX_BYTES];
    };

    struct HandshakeFrame {
        unsigned char nonce[crypto_secretbox_NONCEBYTES];
        unsigned char ciphertext[crypto_secretbox_MACBYTES + sizeof(HandshakePlaintext)];
    };
#pragma pack(pop)
}

CryptoChannel::CryptoChannel() {}

bool CryptoChannel::LoadNetworkKey(std::array<unsigned char, crypto_secretbox_KEYBYTES>& networkKey) {
    std::string errorMessage;
    if (!g_keyManager.GetNetworkKey(networkKey, &errorMessage)) {
        return false;
    }
    return true;
}

bool CryptoChannel::ClientHandshake(const SocketIoContext& io,
                                    const std::array<unsigned char, 
                                    HostIdSize>& localHostId, 
                                    const std::string& localHostNameUtf8, 
                                    std::array<unsigned char, HostIdSize>& remoteHostId, 
                                    std::string& remoteHostNameUtf8)
{
    std::array<unsigned char, crypto_secretbox_KEYBYTES> networkKey{};
    if (!LoadNetworkKey(networkKey)) return false;

    unsigned char clientPk[crypto_kx_PUBLICKEYBYTES]{}; unsigned char clientSk[crypto_kx_SECRETKEYBYTES]{};
    crypto_kx_keypair(clientPk, clientSk);

    HandshakePlaintext plain{};
    std::memcpy(plain.ephemeralPk, clientPk, crypto_kx_PUBLICKEYBYTES);
    std::memcpy(plain.hostId, localHostId.data(), HostIdSize);
    strncpys(plain.hostNameUTF8, localHostNameUtf8.c_str());

    HandshakeFrame tx{}; randombytes_buf(tx.nonce, sizeof(tx.nonce));
    crypto_secretbox_easy(tx.ciphertext, reinterpret_cast<const unsigned char*>(&plain), sizeof(plain), tx.nonce, networkKey.data());
    if (!SendAll(io, reinterpret_cast<const char*>(&tx), sizeof(tx))) return false;

    HandshakeFrame rx{};
    if (!RecvAll(io, reinterpret_cast<char*>(&rx), sizeof(rx))) return false;
    HandshakePlaintext remotePlain{};
    if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(&remotePlain), rx.ciphertext, sizeof(rx.ciphertext), rx.nonce, networkKey.data()) != 0) return false;

    unsigned char serverPk[crypto_kx_PUBLICKEYBYTES]{};
    std::memcpy(serverPk, remotePlain.ephemeralPk, crypto_kx_PUBLICKEYBYTES);
    std::memcpy(remoteHostId.data(), remotePlain.hostId, HostIdSize);
    remoteHostNameUtf8 = remotePlain.hostNameUTF8;

    unsigned char rxKey[crypto_kx_SESSIONKEYBYTES]{}; unsigned char txKey[crypto_kx_SESSIONKEYBYTES]{};
    if (crypto_kx_client_session_keys(rxKey, txKey, clientPk, clientSk, serverPk) != 0) return false;

    unsigned char txHeader[crypto_secretstream_xchacha20poly1305_HEADERBYTES]{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&txState_, txHeader, txKey) != 0) return false;
    if (!SendAll(io, reinterpret_cast<const char*>(txHeader), sizeof(txHeader))) return false;
    unsigned char rxHeader[crypto_secretstream_xchacha20poly1305_HEADERBYTES]{};
    if (!RecvAll(io, reinterpret_cast<char*>(rxHeader), sizeof(rxHeader))) return false;
    if (crypto_secretstream_xchacha20poly1305_init_pull(&rxState_, rxHeader, rxKey) != 0) return false;
    return true;
}

bool CryptoChannel::ServerHandshake(const SocketIoContext& io,
                                    std::array<unsigned char, 
                                    HostIdSize>& remoteHostId, 
                                    std::string& remoteHostNameUtf8)
{
    std::array<unsigned char, crypto_secretbox_KEYBYTES> networkKey{};
    if (!LoadNetworkKey(networkKey)) return false;

    HandshakeFrame rx{};
    if (!RecvAll(io, reinterpret_cast<char*>(&rx), sizeof(rx))) return false;
    HandshakePlaintext remotePlain{};
    if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(&remotePlain), rx.ciphertext, sizeof(rx.ciphertext), rx.nonce, networkKey.data()) != 0) return false;

    unsigned char clientPk[crypto_kx_PUBLICKEYBYTES]{};
    std::memcpy(clientPk, remotePlain.ephemeralPk, crypto_kx_PUBLICKEYBYTES);
    std::memcpy(remoteHostId.data(), remotePlain.hostId, HostIdSize);
    remoteHostNameUtf8 = remotePlain.hostNameUTF8;

    std::array<unsigned char, 32> localHostId{}; if (!g_settings.getHostID(localHostId)) return false;
    char localHostNameUtf8[HOSTNAME_MAX_BYTES] = {};
    if (gethostname(localHostNameUtf8, sizeof(localHostNameUtf8)) != 0) return false;

    unsigned char serverPk[crypto_kx_PUBLICKEYBYTES]{}; unsigned char serverSk[crypto_kx_SECRETKEYBYTES]{};
    crypto_kx_keypair(serverPk, serverSk);
    HandshakePlaintext plain{};
    std::memcpy(plain.ephemeralPk, serverPk, crypto_kx_PUBLICKEYBYTES);
    std::memcpy(plain.hostId, localHostId.data(), HostIdSize);
    strncpys(plain.hostNameUTF8, localHostNameUtf8);
    HandshakeFrame tx{}; randombytes_buf(tx.nonce, sizeof(tx.nonce));
    crypto_secretbox_easy(tx.ciphertext, reinterpret_cast<const unsigned char*>(&plain), sizeof(plain), tx.nonce, networkKey.data());
    if (!SendAll(io, reinterpret_cast<const char*>(&tx), sizeof(tx))) return false;

    unsigned char rxKey[crypto_kx_SESSIONKEYBYTES]{}; unsigned char txKey[crypto_kx_SESSIONKEYBYTES]{};
    if (crypto_kx_server_session_keys(rxKey, txKey, serverPk, serverSk, clientPk) != 0) return false;
    unsigned char rxHeader[crypto_secretstream_xchacha20poly1305_HEADERBYTES]{};
    if (!RecvAll(io, reinterpret_cast<char*>(rxHeader), sizeof(rxHeader))) return false;
    if (crypto_secretstream_xchacha20poly1305_init_pull(&rxState_, rxHeader, rxKey) != 0) return false;
    unsigned char txHeader[crypto_secretstream_xchacha20poly1305_HEADERBYTES]{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&txState_, txHeader, txKey) != 0) return false;
    if (!SendAll(io, reinterpret_cast<const char*>(txHeader), sizeof(txHeader))) return false;
    return true;
}

namespace {
    constexpr uint32_t kMaxCiphertextMessageBytes = 64u * 1024u * 1024u; // 64 MiB
}

bool CryptoChannel::SendMessage(const SocketIoContext& io, const unsigned char* data, uint32_t dataSize) {
    if (dataSize > (kMaxCiphertextMessageBytes - crypto_secretstream_xchacha20poly1305_ABYTES)) return false;
    unsigned long long clen = 0; unsigned char tag = 0;
    std::vector<unsigned char> c(dataSize + crypto_secretstream_xchacha20poly1305_ABYTES);
    if (crypto_secretstream_xchacha20poly1305_push(&txState_, c.data(), &clen, data, dataSize, nullptr, 0, tag) != 0) return false;
    if (clen > kMaxCiphertextMessageBytes) return false;
    const uint32_t n = htonl(static_cast<uint32_t>(clen));
    return SendAll(io, reinterpret_cast<const char*>(&n), sizeof(n)) && SendAll(io, reinterpret_cast<const char*>(c.data()), static_cast<int>(clen));
}

bool CryptoChannel::RecvMessage(const SocketIoContext& io, std::vector<unsigned char>& outData) {
    uint32_t n = 0;
    if (!RecvAll(io, reinterpret_cast<char*>(&n), sizeof(n))) return false;
    const uint32_t clen = ntohl(n);
    if (clen <= crypto_secretstream_xchacha20poly1305_ABYTES || clen > kMaxCiphertextMessageBytes) return false;
    std::vector<unsigned char> c(clen);
    if (!RecvAll(io, reinterpret_cast<char*>(c.data()), static_cast<int>(clen))) return false;
    unsigned long long mlen = 0; unsigned char tag = 0;
    outData.assign(static_cast<size_t>(clen), 0);
    if (crypto_secretstream_xchacha20poly1305_pull(&rxState_, outData.data(), &mlen, &tag, c.data(), c.size(), nullptr, 0) != 0) return false;
    outData.resize(static_cast<size_t>(mlen));
    return true;
}

bool CryptoChannel::SendTaggedMessage(const SocketIoContext& io, const char* tag4) {
    return SendMessage(io, reinterpret_cast<const unsigned char*>(tag4), 4);
}

bool CryptoChannel::RecvTaggedMessage(const SocketIoContext& io, char* outTag4) {
    std::vector<unsigned char> message;
    if (!RecvMessage(io, message) || message.size() != 4) return false;
    std::memcpy(outTag4, message.data(), 4);
    return true;
}
