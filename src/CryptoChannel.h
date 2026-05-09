#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <sodium.h>

#include "platform.h"

struct SocketIoContext;

class CryptoChannel {
public:
    static constexpr size_t HostIdSize = 32;
    static constexpr size_t HOSTNAME_MAX_BYTES = 256;

    using HostId = std::array<unsigned char, HostIdSize>;

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

    bool SendTaggedMessage(const SocketIoContext& io, const char* tag4);
    bool RecvTaggedMessage(const SocketIoContext& io, char* outTag4);

    bool SendMessage(const SocketIoContext& io, const unsigned char* data, uint32_t dataSize);
    bool RecvMessage(const SocketIoContext& io, std::vector<unsigned char>& outData);

private:
    bool LoadNetworkKey(std::array<unsigned char, crypto_secretbox_KEYBYTES>& networkKey);

    crypto_secretstream_xchacha20poly1305_state txState_{};
    crypto_secretstream_xchacha20poly1305_state rxState_{};
};
