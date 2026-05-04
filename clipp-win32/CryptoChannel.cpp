#include "CryptoChannel.h"

#include <vector>
#include <cstring>

#include "Settings.h"
#include "KeyManager.h"

namespace {
#pragma pack(push, 1)
struct HandshakeFrame {
    unsigned char nonce[crypto_secretbox_NONCEBYTES];
    unsigned char ciphertext[crypto_secretbox_MACBYTES + crypto_kx_PUBLICKEYBYTES + 32 + (256 * sizeof(wchar_t))];
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

bool CryptoChannel::RecvAll(SOCKET sock, char* buffer, int length) { int total = 0; while (total < length) { int r = recv(sock, buffer + total, length - total, 0); if (r <= 0) return false; total += r; } return true; }
bool CryptoChannel::SendAll(SOCKET sock, const char* buffer, int length) { int total = 0; while (total < length) { int s = send(sock, buffer + total, length - total, 0); if (s <= 0) return false; total += s; } return true; }

bool CryptoChannel::ClientHandshake(SOCKET socket, const std::array<unsigned char, HostIdSize>& localHostId, const std::wstring& localHostName, std::array<unsigned char, HostIdSize>& remoteHostId, std::wstring& remoteHostName) {
    std::array<unsigned char, crypto_secretbox_KEYBYTES> networkKey{};
    if (!LoadNetworkKey(networkKey)) return false;

    unsigned char clientPk[crypto_kx_PUBLICKEYBYTES]{}; unsigned char clientSk[crypto_kx_SECRETKEYBYTES]{};
    crypto_kx_keypair(clientPk, clientSk);

    unsigned char plain[crypto_kx_PUBLICKEYBYTES + 32 + (256 * sizeof(wchar_t))]{};
    std::memcpy(plain, clientPk, crypto_kx_PUBLICKEYBYTES);
    std::memcpy(plain + crypto_kx_PUBLICKEYBYTES, localHostId.data(), 32);
    std::wmemset(reinterpret_cast<wchar_t*>(plain + crypto_kx_PUBLICKEYBYTES + 32), 0, 256);
    wcsncpy_s(reinterpret_cast<wchar_t*>(plain + crypto_kx_PUBLICKEYBYTES + 32), 256, localHostName.c_str(), _TRUNCATE);

    HandshakeFrame tx{}; randombytes_buf(tx.nonce, sizeof(tx.nonce));
    crypto_secretbox_easy(tx.ciphertext, plain, sizeof(plain), tx.nonce, networkKey.data());
    if (!SendAll(socket, reinterpret_cast<const char*>(&tx), sizeof(tx))) return false;

    HandshakeFrame rx{};
    if (!RecvAll(socket, reinterpret_cast<char*>(&rx), sizeof(rx))) return false;
    unsigned char remotePlain[sizeof(plain)]{};
    if (crypto_secretbox_open_easy(remotePlain, rx.ciphertext, sizeof(rx.ciphertext), rx.nonce, networkKey.data()) != 0) return false;

    unsigned char serverPk[crypto_kx_PUBLICKEYBYTES]{};
    std::memcpy(serverPk, remotePlain, crypto_kx_PUBLICKEYBYTES);
    std::memcpy(remoteHostId.data(), remotePlain + crypto_kx_PUBLICKEYBYTES, 32);
    remoteHostName = reinterpret_cast<wchar_t*>(remotePlain + crypto_kx_PUBLICKEYBYTES + 32);

    unsigned char rxKey[crypto_kx_SESSIONKEYBYTES]{}; unsigned char txKey[crypto_kx_SESSIONKEYBYTES]{};
    if (crypto_kx_client_session_keys(rxKey, txKey, clientPk, clientSk, serverPk) != 0) return false;

    unsigned char txHeader[crypto_secretstream_xchacha20poly1305_HEADERBYTES]{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&txState_, txHeader, txKey) != 0) return false;
    if (!SendAll(socket, reinterpret_cast<const char*>(txHeader), sizeof(txHeader))) return false;
    unsigned char rxHeader[crypto_secretstream_xchacha20poly1305_HEADERBYTES]{};
    if (!RecvAll(socket, reinterpret_cast<char*>(rxHeader), sizeof(rxHeader))) return false;
    if (crypto_secretstream_xchacha20poly1305_init_pull(&rxState_, rxHeader, rxKey) != 0) return false;
    return true;
}

bool CryptoChannel::ServerHandshake(SOCKET socket, std::array<unsigned char, HostIdSize>& remoteHostId, std::wstring& remoteHostName) {
    std::array<unsigned char, crypto_secretbox_KEYBYTES> networkKey{};
    if (!LoadNetworkKey(networkKey)) return false;

    HandshakeFrame rx{};
    if (!RecvAll(socket, reinterpret_cast<char*>(&rx), sizeof(rx))) return false;
    unsigned char remotePlain[crypto_kx_PUBLICKEYBYTES + 32 + (256 * sizeof(wchar_t))]{};
    if (crypto_secretbox_open_easy(remotePlain, rx.ciphertext, sizeof(rx.ciphertext), rx.nonce, networkKey.data()) != 0) return false;

    unsigned char clientPk[crypto_kx_PUBLICKEYBYTES]{};
    std::memcpy(clientPk, remotePlain, crypto_kx_PUBLICKEYBYTES);
    std::memcpy(remoteHostId.data(), remotePlain + crypto_kx_PUBLICKEYBYTES, 32);
    remoteHostName = reinterpret_cast<wchar_t*>(remotePlain + crypto_kx_PUBLICKEYBYTES + 32);

    std::array<unsigned char, 32> localHostId{}; if (!g_settings.getHostID(localHostId)) return false;
    wchar_t localHostNameW[256] = L""; char localHostName[256] = {};
    if (gethostname(localHostName, sizeof(localHostName)) != 0) return false;
    mbstowcs_s(nullptr, localHostNameW, _countof(localHostNameW), localHostName, _TRUNCATE);

    unsigned char serverPk[crypto_kx_PUBLICKEYBYTES]{}; unsigned char serverSk[crypto_kx_SECRETKEYBYTES]{};
    crypto_kx_keypair(serverPk, serverSk);
    unsigned char plain[sizeof(remotePlain)]{};
    std::memcpy(plain, serverPk, crypto_kx_PUBLICKEYBYTES);
    std::memcpy(plain + crypto_kx_PUBLICKEYBYTES, localHostId.data(), 32);
    wcsncpy_s(reinterpret_cast<wchar_t*>(plain + crypto_kx_PUBLICKEYBYTES + 32), 256, localHostNameW, _TRUNCATE);
    HandshakeFrame tx{}; randombytes_buf(tx.nonce, sizeof(tx.nonce));
    crypto_secretbox_easy(tx.ciphertext, plain, sizeof(plain), tx.nonce, networkKey.data());
    if (!SendAll(socket, reinterpret_cast<const char*>(&tx), sizeof(tx))) return false;

    unsigned char rxKey[crypto_kx_SESSIONKEYBYTES]{}; unsigned char txKey[crypto_kx_SESSIONKEYBYTES]{};
    if (crypto_kx_server_session_keys(rxKey, txKey, serverPk, serverSk, clientPk) != 0) return false;
    unsigned char rxHeader[crypto_secretstream_xchacha20poly1305_HEADERBYTES]{};
    if (!RecvAll(socket, reinterpret_cast<char*>(rxHeader), sizeof(rxHeader))) return false;
    if (crypto_secretstream_xchacha20poly1305_init_pull(&rxState_, rxHeader, rxKey) != 0) return false;
    unsigned char txHeader[crypto_secretstream_xchacha20poly1305_HEADERBYTES]{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&txState_, txHeader, txKey) != 0) return false;
    if (!SendAll(socket, reinterpret_cast<const char*>(txHeader), sizeof(txHeader))) return false;
    return true;
}

bool CryptoChannel::SendTaggedMessage(SOCKET socket, const char* tag4) {
    unsigned long long clen = 0; unsigned char tag = 0;
    unsigned char c[4 + crypto_secretstream_xchacha20poly1305_ABYTES]{};
    if (crypto_secretstream_xchacha20poly1305_push(&txState_, c, &clen, reinterpret_cast<const unsigned char*>(tag4), 4, nullptr, 0, tag) != 0) return false;
    unsigned short n = htons(static_cast<unsigned short>(clen));
    return SendAll(socket, reinterpret_cast<const char*>(&n), sizeof(n)) && SendAll(socket, reinterpret_cast<const char*>(c), static_cast<int>(clen));
}

bool CryptoChannel::RecvTaggedMessage(SOCKET socket, char* outTag4) {
    unsigned short n = 0;
    if (!RecvAll(socket, reinterpret_cast<char*>(&n), sizeof(n))) return false;
    int clen = ntohs(n);
    if (clen <= crypto_secretstream_xchacha20poly1305_ABYTES || clen > 256) return false;
    std::vector<unsigned char> c(clen);
    if (!RecvAll(socket, reinterpret_cast<char*>(c.data()), clen)) return false;
    unsigned long long mlen = 0; unsigned char tag = 0; unsigned char m[8]{};
    if (crypto_secretstream_xchacha20poly1305_pull(&rxState_, m, &mlen, &tag, c.data(), c.size(), nullptr, 0) != 0) return false;
    if (mlen != 4) return false;
    std::memcpy(outTag4, m, 4);
    return true;
}
