#include "CryptoChannel.h"
#include "RegisterConfig.h"

#include <cstring>
#include <vector>

#include "KeyManager.h"
#include "LocalPeerName.h"
#include "Settings.h"
#include "platform.h"
#include "utils.h"
#include "utils_socket.h"

namespace {
    constexpr uint32_t kMaxCiphertextMessageBytes = 64u * 1024u * 1024u; // 64 MiB
    constexpr char kHandshakeDoneTag[] = "DONE";

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
        unsigned char hostId[HostId::kSize];
        // Feature-capability bits (see CryptoChannel::CAP0_*). Caps gate is-it-safe-
        // to-send decisions for newer frame types and message extensions without
        // breaking peers that predate them. Authentication relies on the surrounding
        // secretbox MAC.
        uint8_t       caps[CryptoChannel::CAPS_BYTES];
        // Sender's OsType (network order). Lives in 2 bytes peeled off caps, so the
        // struct size is unchanged and peers predating this field interoperate (they
        // send zero here -> decoded as OsType::Unknown).
        uint16_t      osType;
        char          hostNameUTF8[CryptoChannel::HOSTNAME_MAX_BYTES];
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
        const HostId& hostId,
        const CryptoChannel::Caps& caps,
        const char* hostNameUtf8)
    {
        std::memcpy(plaintext.ephemeralPk, publicKey.data(), publicKey.size());
        std::memcpy(plaintext.hostId, hostId.data().data(), hostId.data().size());
        std::memcpy(plaintext.caps, caps.data(), caps.size());
        plaintext.osType = htons(static_cast<uint16_t>(GetLocalOsType()));
        strncpys(plaintext.hostNameUTF8, hostNameUtf8);
    }

    PublicKey CopyPublicKey(const HandshakePlaintext& plaintext) {
        PublicKey publicKey{};
        std::memcpy(publicKey.data(), plaintext.ephemeralPk, publicKey.size());
        return publicKey;
    }

    void CopyRemoteIdentity(
        const HandshakePlaintext& plaintext,
        HostId& remoteHostId,
        CryptoChannel::Caps& remoteCaps,
        OsType& remoteOsType,
        std::string& remoteHostNameUtf8)
    {
        std::memcpy(remoteHostId.data().data(), plaintext.hostId, remoteHostId.data().size());
        std::memcpy(remoteCaps.data(), plaintext.caps, remoteCaps.size());
        remoteOsType = static_cast<OsType>(ntohs(plaintext.osType));
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

    bool LoadLocalIdentity(HostId& hostId, std::array<char, CryptoChannel::HOSTNAME_MAX_BYTES>& hostNameUtf8) {
        if (!g_settings.getHostID(hostId)) {
            return false;
        }

        return clipp::CopyLocalPeerDisplayName(hostNameUtf8.data(), hostNameUtf8.size());
    }

    CryptoChannel::Caps LocalCaps() {
        // Advertise the capabilities this build understands. Safe to advertise
        // unconditionally: a peer only acts on a cap when it initiates the matching
        // request, and clients that never accept inbound requests (the CLI verbs,
        // the iOS share extension) simply never get asked.
        CryptoChannel::Caps caps{};
        caps[0] |= CryptoChannel::CAP0_SERVES_RECENT;
#if CLIPP_REGISTERS_DAEMON
        caps[0] |= CryptoChannel::CAP0_SERVES_REGISTERS;
        caps[0] |= CryptoChannel::CAP0_SERVES_PUT;
#endif
        return caps;
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

    bool IsCiphertextMessageSizeValid(uint32_t ciphertextSize) {
        return ciphertextSize > crypto_secretstream_xchacha20poly1305_ABYTES
            && ciphertextSize <= kMaxCiphertextMessageBytes;
    }

    bool TagEquals(const unsigned char* lhs, const char* rhs) {
        return std::memcmp(lhs, rhs, 4) == 0;
    }
}

CryptoChannel::CryptoChannel() = default;

bool CryptoChannel::ClientHandshake(
    const SocketIoContext& io,
    const HostId& localHostId,
    const std::string& localHostNameUtf8,
    HostId& remoteHostId,
    std::string& remoteHostNameUtf8)
{
    NetworkKey clientToServerKey{};
    NetworkKey serverToClientKey{};
    std::string keyErrorMessage;
    if (!g_keyManager.GetKey(KeyManager::KeyRole::TcpHandshakeClientToServer, clientToServerKey, &keyErrorMessage)
        || !g_keyManager.GetKey(KeyManager::KeyRole::TcpHandshakeServerToClient, serverToClientKey, &keyErrorMessage)) {
        return false;
    }

    KeyPair clientKeys{};
    GenerateKeyPair(clientKeys);

    HandshakePlaintext localPlaintext{};
    FillHandshakePlaintext(localPlaintext, clientKeys.publicKey, localHostId, LocalCaps(), localHostNameUtf8.c_str());
    if (!SendHandshakeFrame(io, localPlaintext, clientToServerKey)) {
        return false;
    }

    HandshakePlaintext remotePlaintext{};
    if (!ReceiveHandshakeFrame(io, serverToClientKey, remotePlaintext)) {
        return false;
    }

    const PublicKey serverPublicKey = CopyPublicKey(remotePlaintext);
    CopyRemoteIdentity(remotePlaintext, remoteHostId, remoteCaps_, remoteOsType_, remoteHostNameUtf8);

    SessionKeys sessionKeys{};
    if (!DeriveClientSessionKeys(clientKeys, serverPublicKey, sessionKeys)) {
        return false;
    }

    return SendStreamHeader(io, txState_, sessionKeys.tx)
        && ReceiveStreamHeader(io, rxState_, sessionKeys.rx)
        && SendHandshakeDone(io);
}

bool CryptoChannel::ServerHandshake(
    const SocketIoContext& io,
    HostId& remoteHostId,
    std::string& remoteHostNameUtf8)
{
    NetworkKey clientToServerKey{};
    NetworkKey serverToClientKey{};
    std::string keyErrorMessage;
    if (!g_keyManager.GetKey(KeyManager::KeyRole::TcpHandshakeClientToServer, clientToServerKey, &keyErrorMessage)
        || !g_keyManager.GetKey(KeyManager::KeyRole::TcpHandshakeServerToClient, serverToClientKey, &keyErrorMessage)) {
        return false;
    }

    HandshakePlaintext remotePlaintext{};
    if (!ReceiveHandshakeFrame(io, clientToServerKey, remotePlaintext)) {
        return false;
    }

    const PublicKey clientPublicKey = CopyPublicKey(remotePlaintext);
    CopyRemoteIdentity(remotePlaintext, remoteHostId, remoteCaps_, remoteOsType_, remoteHostNameUtf8);

    HostId localHostId;
    std::array<char, HOSTNAME_MAX_BYTES> localHostNameUtf8{};
    if (!LoadLocalIdentity(localHostId, localHostNameUtf8)) {
        return false;
    }

    KeyPair serverKeys{};
    GenerateKeyPair(serverKeys);

    HandshakePlaintext localPlaintext{};
    FillHandshakePlaintext(localPlaintext, serverKeys.publicKey, localHostId, LocalCaps(), localHostNameUtf8.data());
    if (!SendHandshakeFrame(io, localPlaintext, serverToClientKey)) {
        return false;
    }

    SessionKeys sessionKeys{};
    if (!DeriveServerSessionKeys(serverKeys, clientPublicKey, sessionKeys)) {
        return false;
    }

    return ReceiveStreamHeader(io, rxState_, sessionKeys.rx)
        && SendStreamHeader(io, txState_, sessionKeys.tx)
        && ReceiveHandshakeDone(io);
}

bool CryptoChannel::SendHandshakeDone(const SocketIoContext& io) {
    return SendFrame(io, kHandshakeDoneTag);
}

bool CryptoChannel::ReceiveHandshakeDone(const SocketIoContext& io) {
    std::vector<unsigned char> frame;
    if (!RecvFrame(io, frame) || frame.size() != 4) {
        return false;
    }
    return TagEquals(frame.data(), kHandshakeDoneTag);
}

bool CryptoChannel::SendFrame(const SocketIoContext& io,
                              const char* tag4,
                              const unsigned char* bodyA, uint32_t bodyASize,
                              const unsigned char* bodyB, uint32_t bodyBSize) {
    // 4 (tag) + bodyA + bodyB must fit within the AEAD's plaintext budget.
    constexpr uint64_t kMaxPlaintext =
        static_cast<uint64_t>(kMaxCiphertextMessageBytes) - crypto_secretstream_xchacha20poly1305_ABYTES;
    const uint64_t plaintextSize64 = 4ull
        + static_cast<uint64_t>(bodyASize)
        + static_cast<uint64_t>(bodyBSize);
    if (plaintextSize64 > kMaxPlaintext) {
        return false;
    }
    const size_t plaintextSize = static_cast<size_t>(plaintextSize64);

    sendScratch_.resize(plaintextSize);
    std::memcpy(sendScratch_.data(), tag4, 4);
    if (bodyA != nullptr && bodyASize > 0) {
        std::memcpy(sendScratch_.data() + 4, bodyA, bodyASize);
    }
    if (bodyB != nullptr && bodyBSize > 0) {
        std::memcpy(sendScratch_.data() + 4 + bodyASize, bodyB, bodyBSize);
    }

    std::vector<unsigned char> ciphertext(plaintextSize + crypto_secretstream_xchacha20poly1305_ABYTES);
    unsigned long long ciphertextSize = 0;
    const unsigned char tag = 0;
    if (crypto_secretstream_xchacha20poly1305_push(
            &txState_,
            ciphertext.data(),
            &ciphertextSize,
            sendScratch_.data(),
            static_cast<unsigned long long>(plaintextSize),
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

bool CryptoChannel::RecvFrame(const SocketIoContext& io, std::vector<unsigned char>& outPlaintext) {
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

    outPlaintext.assign(static_cast<size_t>(ciphertextSize), 0);
    unsigned long long plaintextSize = 0;
    unsigned char tag = 0;
    if (crypto_secretstream_xchacha20poly1305_pull(
            &rxState_,
            outPlaintext.data(),
            &plaintextSize,
            &tag,
            ciphertext.data(),
            ciphertext.size(),
            nullptr,
            0) != 0) {
        return false;
    }

    outPlaintext.resize(static_cast<size_t>(plaintextSize));
    return true;
}
