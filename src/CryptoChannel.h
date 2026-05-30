#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <sodium.h>

#include "platform.h"
#include "HostId.h"

struct SocketIoContext;

class CryptoChannel {
public:
    static constexpr size_t HOSTNAME_MAX_BYTES = 128;
    static constexpr size_t CAPS_BYTES = 16;
    using Caps = std::array<uint8_t, CAPS_BYTES>;

    // Capability bits advertised in the handshake (Caps[0]). SERVES_RECENT means
    // this peer answers an "RCNT" frame with its most recent clipboard item — used
    // by one-shot clients like `clipp paste` to decide whether a peer is worth asking.
    static constexpr uint8_t CAP0_SERVES_RECENT = 0x01;

    CryptoChannel();

    bool ClientHandshake(
        const SocketIoContext& io,
        const HostId& localHostId,
        const std::string& localHostNameUtf8,
        HostId& remoteHostId,
        std::string& remoteHostNameUtf8);

    bool ServerHandshake(
        const SocketIoContext& io,
        HostId& remoteHostId,
        std::string& remoteHostNameUtf8);

    // Send a single secretstream frame: tag(4) + bodyA + bodyB, concatenated and
    // encrypted in one push. The second buffer is optional; defaults to no body.
    // The internal scratch buffer grows but doesn't shrink across calls.
    bool SendFrame(const SocketIoContext& io,
                   const char* tag4,
                   const unsigned char* bodyA = nullptr,
                   uint32_t bodyASize = 0,
                   const unsigned char* bodyB = nullptr,
                   uint32_t bodyBSize = 0);

    // Receive one secretstream frame. On success, outPlaintext.size() >= 4; the
    // first 4 bytes are the tag, the remainder is the body. Callers slice in place.
    bool RecvFrame(const SocketIoContext& io, std::vector<unsigned char>& outPlaintext);

    const Caps& RemoteCaps() const { return remoteCaps_; }

private:
    bool SendHandshakeDone(const SocketIoContext& io);
    bool ReceiveHandshakeDone(const SocketIoContext& io);

    crypto_secretstream_xchacha20poly1305_state txState_{};
    crypto_secretstream_xchacha20poly1305_state rxState_{};
    std::vector<unsigned char> sendScratch_;
    Caps remoteCaps_{};
};
