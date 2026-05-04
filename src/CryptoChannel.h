#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <sodium.h>
#include <vector>
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <cerrno>
    using SOCKET=int;
    const int INVALID_SOCKET = -1;
    const int SOCKET_ERROR = -1;
    static inline void closesocket(SOCKET s) { close(s); }
#endif

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

    bool SendMessage(SOCKET socket, const unsigned char* data, uint32_t dataSize);
    bool RecvMessage(SOCKET socket, std::vector<unsigned char>& outData);

private:
    bool LoadNetworkKey(std::array<unsigned char, crypto_secretbox_KEYBYTES>& networkKey);
    static bool RecvAll(SOCKET sock, char* buffer, int length);
    static bool SendAll(SOCKET sock, const char* buffer, int length);

    crypto_secretstream_xchacha20poly1305_state txState_{};
    crypto_secretstream_xchacha20poly1305_state rxState_{};
};
