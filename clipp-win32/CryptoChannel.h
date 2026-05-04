#pragma once

#include <array>
#include <string>
#include <winsock2.h>
#include <sodium.h>

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
                        std::string& remoteHostNameUtf8);
    bool ServerHandshake(SOCKET socket, 
                        std::array<unsigned char, 
                        HostIdSize>& remoteHostId, 
                        std::string& remoteHostNameUtf8);

    bool SendTaggedMessage(SOCKET socket, const char* tag4);
    bool RecvTaggedMessage(SOCKET socket, char* outTag4);

private:
    bool LoadNetworkKey(std::array<unsigned char, crypto_secretbox_KEYBYTES>& networkKey);
    static bool RecvAll(SOCKET sock, char* buffer, int length);
    static bool SendAll(SOCKET sock, const char* buffer, int length);

    crypto_secretstream_xchacha20poly1305_state txState_{};
    crypto_secretstream_xchacha20poly1305_state rxState_{};
};
