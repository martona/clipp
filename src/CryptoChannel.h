#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <sodium.h>
#include <vector>
#include "platform.h"

class SocketWakeEvent;

class CryptoChannel {
public:
    static constexpr size_t HostIdSize = 32;
    static constexpr size_t HOSTNAME_MAX_BYTES = 256;

    CryptoChannel();

    bool ClientHandshake(SOCKET socket, 
                        const std::array<unsigned char, 
                        HostIdSize>& localHostId, 
                        const std::string& localHostNameUtf8, 
                        std::array<unsigned char, 
                        HostIdSize>& remoteHostId, 
                        std::string& remoteHostNameUtf8,
                        const SocketWakeEvent* wakeEvent = nullptr);
    bool ServerHandshake(SOCKET socket, 
                        std::array<unsigned char, 
                        HostIdSize>& remoteHostId, 
                        std::string& remoteHostNameUtf8,
                        const SocketWakeEvent* wakeEvent = nullptr);

    bool SendTaggedMessage(SOCKET socket, const char* tag4, const SocketWakeEvent* wakeEvent = nullptr);
    bool RecvTaggedMessage(SOCKET socket, char* outTag4, const SocketWakeEvent* wakeEvent = nullptr);

    bool SendMessage(SOCKET socket, const unsigned char* data, uint32_t dataSize, const SocketWakeEvent* wakeEvent = nullptr);
    bool RecvMessage(SOCKET socket, std::vector<unsigned char>& outData, const SocketWakeEvent* wakeEvent = nullptr);

private:
    bool LoadNetworkKey(std::array<unsigned char, crypto_secretbox_KEYBYTES>& networkKey);

    crypto_secretstream_xchacha20poly1305_state txState_{};
    crypto_secretstream_xchacha20poly1305_state rxState_{};
};
