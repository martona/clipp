#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <sodium.h>

#include "platform.h"
#include "HostId.h"
#include "OsType.h"

struct SocketIoContext;

class CryptoChannel {
public:
    static constexpr size_t HOSTNAME_MAX_BYTES = 128;
    // Was 16; 2 bytes were peeled off for the dedicated osType field in
    // HandshakePlaintext (see CryptoChannel.cpp). The on-wire struct size is
    // unchanged, so this stays interoperable with peers that predate osType.
    static constexpr size_t CAPS_BYTES = 14;
    using Caps = std::array<uint8_t, CAPS_BYTES>;

    // Capability bits advertised in the handshake (Caps[0]). SERVES_RECENT means
    // this peer answers an "RCNT" frame with its most recent clipboard item — used
    // by one-shot clients like `clipp paste` to decide whether a peer is worth asking.
    static constexpr uint8_t CAP0_SERVES_RECENT = 0x01;
    // SERVES_REGISTERS means this peer participates in the named-register protocol
    // (REGW/RGET/RLST/RSYN anti-entropy). Advertised only by desktop daemons — the
    // one-shot CLI and iOS don't serve. Gating register-frame sends on it keeps old
    // and non-serving peers from seeing frames they'd only log-and-ignore.
    static constexpr uint8_t CAP0_SERVES_REGISTERS = 0x02;
    // SERVES_PUT means this peer answers "RPUT <name>" (CLI `put`) by promoting that
    // named register to the live clipboard itself — apply + mesh broadcast + ""
    // mirror — acked with ROKP/NONE. Against peers without it, the CLI falls back
    // to doing the same via RGET followed by a relay CLIP.
    static constexpr uint8_t CAP0_SERVES_PUT = 0x04;

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

    // The remote's self-reported OS, learned from the handshake. OsType::Unknown
    // for peers that predate the field (they send those bytes as zero).
    OsType RemoteOsType() const { return remoteOsType_; }

private:
    bool SendHandshakeDone(const SocketIoContext& io);
    bool ReceiveHandshakeDone(const SocketIoContext& io);

    crypto_secretstream_xchacha20poly1305_state txState_{};
    crypto_secretstream_xchacha20poly1305_state rxState_{};
    std::vector<unsigned char> sendScratch_;
    Caps remoteCaps_{};
    OsType remoteOsType_{ OsType::Unknown };
};
