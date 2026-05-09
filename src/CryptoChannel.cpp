#include "CryptoChannel.h"

#include <cstring>
#include <vector>

#include "KeyManager.h"
#include "Settings.h"
#include "platform.h"
#include "utils.h"
#include "utils_socket.h"

namespace {
    constexpr uint32_t kMaxCiphertextMessageBytes = 64u * 1024u * 1024u; // 64 MiB

    using NetworkKey = std::array<unsigned char, crypto_secretbox_KEYBYTES>;
    using PublicKey = std::array<unsigned char, crypto_kx_PUBLICKEYBYTES>;
    using SecretKey = std::array<unsigned char, crypto_kx_SECRETKEYBYTES>;
    using SessionKey = std::array<unsigned char, crypto_kx_SESSIONKEYBYTES>;
    using StreamHeader = std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES>;

    struct KeyPair {
        PublicKey publicKey{};
        SecretKey secretKey{};
    };

    struct SessionKeys {
        SessionKey rx{};
        SessionKey tx{};
    };

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

    void GenerateKeyPair(KeyPair& keyPair) {
        crypto_kx_keypair(keyPair.publicKey.data(), keyPair.secretKey.data());
    }

    void FillHandshakePlaintext(
        HandshakePlaintext& plaintext,
        const PublicKey& publicKey,
        const CryptoChannel::HostId& hostId,
        const char* hostNameUtf8)
    {
        std::memcpy(plaintext.ephemeralPk, publicKey.data(), publicKey.size());
        std::memcpy(plaintext.hostId, hostId.data(), hostId.size());
        strncpys(plaintext.hostNameUTF8, hostNameUtf8);
    }

    PublicKey CopyPublicKey(const HandshakePlaintext& plaintext) {
        PublicKey publicKey{};
        std::memcpy(publicKey.data(), plaintext.ephemeralPk, publicKey.size());
        return publicKey;
    }

    void CopyRemoteIdentity(
        const HandshakePlaintext& plaintext,
        CryptoChannel::HostId& remoteHostId,
        std::string& remoteHostNameUtf8)
    {
        std::memcpy(remoteHostId.data(), plaintext.hostId, remoteHostId.size());
        remoteHostNameUtf8 = plaintext.hostNameUTF8;
    }

    void SealHandshakeFrame(
        const HandshakePlaintext& plaintext,
        const NetworkKey& networkKey,
        HandshakeFrame& frame)
    {
        randombytes_buf(frame.nonce, sizeof(frame.nonce));
        crypto_secretbox_easy(
            frame.ciphertext,
            reinterpret_cast<const unsigned char*>(&plaintext),
            sizeof(plaintext),
            frame.nonce,
            networkKey.data());
    }

    bool OpenHandshakeFrame(
        const HandshakeFrame& frame,
        const NetworkKey& networkKey,
        HandshakePlaintext& plaintext)
    {
        return crypto_secretbox_open_easy(
            reinterpret_cast<unsigned char*>(&plaintext),
            frame.ciphertext,
            sizeof(frame.ciphertext),
            frame.nonce,
            networkKey.data()) == 0;
    }

    bool SendHandshakeFrame(
        const SocketIoContext& io,
        const HandshakePlaintext& plaintext,
        const NetworkKey& networkKey)
    {
        HandshakeFrame frame{};
        SealHandshakeFrame(plaintext, networkKey, frame);
        return SendAll(io, reinterpret_cast<const char*>(&frame), sizeof(frame));
    }

    bool ReceiveHandshakeFrame(
        const SocketIoContext& io,
        const NetworkKey& networkKey,
        HandshakePlaintext& plaintext)
    {
        HandshakeFrame frame{};
        if (!RecvAll(io, reinterpret_cast<char*>(&frame), sizeof(frame))) {
            return false;
        }

        return OpenHandshakeFrame(frame, networkKey, plaintext);
    }

    bool LoadLocalIdentity(CryptoChannel::HostId& hostId, std::array<char, CryptoChannel::HOSTNAME_MAX_BYTES>& hostNameUtf8) {
        if (!g_settings.getHostID(hostId)) {
            return false;
        }

        return gethostname(hostNameUtf8.data(), static_cast<int>(hostNameUtf8.size())) == 0;
    }

    bool DeriveClientSessionKeys(
        const KeyPair& clientKeys,
        const PublicKey& serverPublicKey,
        SessionKeys& sessionKeys)
    {
        return crypto_kx_client_session_keys(
            sessionKeys.rx.data(),
            sessionKeys.tx.data(),
            clientKeys.publicKey.data(),
            clientKeys.secretKey.data(),
            serverPublicKey.data()) == 0;
    }

    bool DeriveServerSessionKeys(
        const KeyPair& serverKeys,
        const PublicKey& clientPublicKey,
        SessionKeys& sessionKeys)
    {
        return crypto_kx_server_session_keys(
            sessionKeys.rx.data(),
            sessionKeys.tx.data(),
            serverKeys.publicKey.data(),
            serverKeys.secretKey.data(),
            clientPublicKey.data()) == 0;
    }

    bool SendStreamHeader(
        const SocketIoContext& io,
        crypto_secretstream_xchacha20poly1305_state& state,
        const SessionKey& txKey)
    {
        StreamHeader header{};
        if (crypto_secretstream_xchacha20poly1305_init_push(&state, header.data(), txKey.data()) != 0) {
            return false;
        }

        return SendAll(io, reinterpret_cast<const char*>(header.data()), static_cast<int>(header.size()));
    }

    bool ReceiveStreamHeader(
        const SocketIoContext& io,
        crypto_secretstream_xchacha20poly1305_state& state,
        const SessionKey& rxKey)
    {
        StreamHeader header{};
        if (!RecvAll(io, reinterpret_cast<char*>(header.data()), static_cast<int>(header.size()))) {
            return false;
        }

        return crypto_secretstream_xchacha20poly1305_init_pull(&state, header.data(), rxKey.data()) == 0;
    }

    bool CanEncryptMessage(uint32_t dataSize) {
        return dataSize <= (kMaxCiphertextMessageBytes - crypto_secretstream_xchacha20poly1305_ABYTES);
    }

    bool IsCiphertextMessageSizeValid(uint32_t ciphertextSize) {
        return ciphertextSize > crypto_secretstream_xchacha20poly1305_ABYTES
            && ciphertextSize <= kMaxCiphertextMessageBytes;
    }
}

CryptoChannel::CryptoChannel() = default;

bool CryptoChannel::LoadNetworkKey(std::array<unsigned char, crypto_secretbox_KEYBYTES>& networkKey) {
    std::string errorMessage;
    return g_keyManager.GetNetworkKey(networkKey, &errorMessage);
}

bool CryptoChannel::ClientHandshake(
    const SocketIoContext& io,
    const HostId& localHostId,
    const std::string& localHostNameUtf8,
    HostId& remoteHostId,
    std::string& remoteHostNameUtf8)
{
    NetworkKey networkKey{};
    if (!LoadNetworkKey(networkKey)) {
        return false;
    }

    KeyPair clientKeys{};
    GenerateKeyPair(clientKeys);

    HandshakePlaintext localPlaintext{};
    FillHandshakePlaintext(localPlaintext, clientKeys.publicKey, localHostId, localHostNameUtf8.c_str());
    if (!SendHandshakeFrame(io, localPlaintext, networkKey)) {
        return false;
    }

    HandshakePlaintext remotePlaintext{};
    if (!ReceiveHandshakeFrame(io, networkKey, remotePlaintext)) {
        return false;
    }

    const PublicKey serverPublicKey = CopyPublicKey(remotePlaintext);
    CopyRemoteIdentity(remotePlaintext, remoteHostId, remoteHostNameUtf8);

    SessionKeys sessionKeys{};
    if (!DeriveClientSessionKeys(clientKeys, serverPublicKey, sessionKeys)) {
        return false;
    }

    return SendStreamHeader(io, txState_, sessionKeys.tx)
        && ReceiveStreamHeader(io, rxState_, sessionKeys.rx);
}

bool CryptoChannel::ServerHandshake(
    const SocketIoContext& io,
    HostId& remoteHostId,
    std::string& remoteHostNameUtf8)
{
    NetworkKey networkKey{};
    if (!LoadNetworkKey(networkKey)) {
        return false;
    }

    HandshakePlaintext remotePlaintext{};
    if (!ReceiveHandshakeFrame(io, networkKey, remotePlaintext)) {
        return false;
    }

    const PublicKey clientPublicKey = CopyPublicKey(remotePlaintext);
    CopyRemoteIdentity(remotePlaintext, remoteHostId, remoteHostNameUtf8);

    HostId localHostId{};
    std::array<char, HOSTNAME_MAX_BYTES> localHostNameUtf8{};
    if (!LoadLocalIdentity(localHostId, localHostNameUtf8)) {
        return false;
    }

    KeyPair serverKeys{};
    GenerateKeyPair(serverKeys);

    HandshakePlaintext localPlaintext{};
    FillHandshakePlaintext(localPlaintext, serverKeys.publicKey, localHostId, localHostNameUtf8.data());
    if (!SendHandshakeFrame(io, localPlaintext, networkKey)) {
        return false;
    }

    SessionKeys sessionKeys{};
    if (!DeriveServerSessionKeys(serverKeys, clientPublicKey, sessionKeys)) {
        return false;
    }

    return ReceiveStreamHeader(io, rxState_, sessionKeys.rx)
        && SendStreamHeader(io, txState_, sessionKeys.tx);
}

bool CryptoChannel::SendMessage(const SocketIoContext& io, const unsigned char* data, uint32_t dataSize) {
    if (!CanEncryptMessage(dataSize)) {
        return false;
    }

    std::vector<unsigned char> ciphertext(dataSize + crypto_secretstream_xchacha20poly1305_ABYTES);
    unsigned long long ciphertextSize = 0;
    unsigned char tag = 0;

    if (crypto_secretstream_xchacha20poly1305_push(
            &txState_,
            ciphertext.data(),
            &ciphertextSize,
            data,
            dataSize,
            nullptr,
            0,
            tag) != 0) {
        return false;
    }

    if (ciphertextSize > kMaxCiphertextMessageBytes) {
        return false;
    }

    const uint32_t networkSize = htonl(static_cast<uint32_t>(ciphertextSize));
    return SendAll(io, reinterpret_cast<const char*>(&networkSize), sizeof(networkSize))
        && SendAll(io, reinterpret_cast<const char*>(ciphertext.data()), static_cast<int>(ciphertextSize));
}

bool CryptoChannel::RecvMessage(const SocketIoContext& io, std::vector<unsigned char>& outData) {
    uint32_t networkSize = 0;
    if (!RecvAll(io, reinterpret_cast<char*>(&networkSize), sizeof(networkSize))) {
        return false;
    }

    const uint32_t ciphertextSize = ntohl(networkSize);
    if (!IsCiphertextMessageSizeValid(ciphertextSize)) {
        return false;
    }

    std::vector<unsigned char> ciphertext(ciphertextSize);
    if (!RecvAll(io, reinterpret_cast<char*>(ciphertext.data()), static_cast<int>(ciphertext.size()))) {
        return false;
    }

    unsigned long long plaintextSize = 0;
    unsigned char tag = 0;
    outData.assign(static_cast<size_t>(ciphertextSize), 0);

    if (crypto_secretstream_xchacha20poly1305_pull(
            &rxState_,
            outData.data(),
            &plaintextSize,
            &tag,
            ciphertext.data(),
            ciphertext.size(),
            nullptr,
            0) != 0) {
        return false;
    }

    outData.resize(static_cast<size_t>(plaintextSize));
    return true;
}

bool CryptoChannel::SendTaggedMessage(const SocketIoContext& io, const char* tag4) {
    return SendMessage(io, reinterpret_cast<const unsigned char*>(tag4), 4);
}

bool CryptoChannel::RecvTaggedMessage(const SocketIoContext& io, char* outTag4) {
    std::vector<unsigned char> message;
    if (!RecvMessage(io, message) || message.size() != 4) {
        return false;
    }

    std::memcpy(outTag4, message.data(), 4);
    return true;
}
