// clipp.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <cstdlib>
#include <iostream>
#include <sodium.h>
#include <string>

#include "platform.h"

#include "Logger.h"
#include "KeyManager.h"
#include "MDNSThread.h"
#include "Listener.h"
#include "Peer.h"
#include "PeerManager.h"
#include "Clipboard.h"

#ifndef _WIN32
    #include <termios.h>
    #include <unistd.h>
#endif

Settings g_settings;
PeerManager g_peerManager;

static std::string ReadHiddenLine(const std::string & prompt) {
    std::cout << prompt.c_str();
    std::string input;

    #ifdef _WIN32
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hStdin, &mode);
        SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));

        std::getline(std::cin, input);

        SetConsoleMode(hStdin, mode);
    #else
        termios oldt;
        tcgetattr(STDIN_FILENO, &oldt);

        termios newt = oldt;
        newt.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);

        std::getline(std::cin, input);

        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    #endif

    std::cout << std::endl;
    return input;
}

void OnClipboardNotification(PlatformWindowHandle hwnd) {
    g_logger.log(__FUNCTION__, Logger::Level::Info, "Clipboard notification received");
    auto clipboardData = ReadClipboardData(hwnd);
    if (clipboardData.formatId == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Clipboard is empty or contains unsupported format");
        return;
	}
	clipboardData.ZstdCompress();
	auto payload = std::make_shared<const ClipboardPayload>(clipboardData);
    g_peerManager.BroadcastClipboard(payload);
	g_logger.log(__FUNCTION__, Logger::Level::Info, "Broadcasted clipboard data to peers (format ID: %u, size: %zu bytes)", clipboardData.formatId, clipboardData.rawData.size());
}

Listener g_listener([](const std::wstring& hostName, const std::array<unsigned char, 32>& hostID, ClipboardPayload& payload) {
    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Received clipboard data from client %s (format ID: %u, size: %zu bytes)", hostName.c_str(), payload.formatId, payload.rawData.size());
    if (!payload.ZstdDecompress()) return;
    SetClipboardData(payload);
});

void OnMDNSNotification(const char* hostNameUtf8, 
                        const char* hostID, 
                        const char* senderIp, 
                        const char* queryID, 
                        const char* nonce, 
                        const char* verb, 
                        u_short port, 
                        const unsigned char* rawHostID) 
{
	g_logger.log(__FUNCTION__, Logger::Level::Info, 
        "mDNS notification received for host: %s / %s\n  from: %s:%hu\n  verb:    %s\n  queryID: %s\n  nonce:   %s", 
        hostNameUtf8, hostID, senderIp, port, verb, queryID, nonce);

    if (/*std::string(verb) == "response" &&*/rawHostID != nullptr) {
        size_t hostNameWLen = utf8_to_utf16(hostNameUtf8, strlen(hostNameUtf8), nullptr, 0);
        std::wstring hostNameW(hostNameWLen > 0 ? hostNameWLen - 1 : 0, L'\0');
        if (hostNameWLen > 1) {
            utf8_to_utf16(hostNameUtf8, strlen(hostNameUtf8), hostNameW.data(), hostNameWLen);
        }
        g_peerManager.AddPeer(hostNameW.c_str(), rawHostID, senderIp, port);
    }

    g_peerManager.CullPeers();
}

void PrintNetworkKeyHash(const std::array<unsigned char, KeyManager::NetworkKeySize>& networkKey) {
    unsigned char keyHash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(keyHash, networkKey.data(), networkKey.size());
    char keyHashHex[crypto_hash_sha256_BYTES * 2 + 1];
    sodium_bin2hex(keyHashHex, sizeof(keyHashHex), keyHash, sizeof(keyHash));
    g_logger.log(__FUNCTION__, Logger::Level::Info, "Network Key SHA-256: %s", keyHashHex);
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "setkey") {
        std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
        const std::string keyInput = ReadHiddenLine("Enter 64-character network key hex: ");

        if (!g_keyManager.ParseHexNetworkKey(keyInput, networkKey)) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Invalid input. Expected exactly 64 hexadecimal characters.");
            return 1;
        }
		PrintNetworkKeyHash(networkKey);

        std::string errorMessage;
        if (!g_keyManager.SetNetworkKey(networkKey, &errorMessage)) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to store network key: %s", errorMessage.c_str());
            return 1;
        }

        g_logger.log(__FUNCTION__, Logger::Level::Info, "Network key saved successfully.");
        return 0;
    }

    #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return -1;
        }
    #endif

    // libsodium requires initialization before calling any other functions
    if (sodium_init() < 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Fatal: libsodium failed to initialize!");
        return 1;
    }
    g_logger.log(__FUNCTION__, Logger::Level::Info, "libsodium initialized successfully.");

    std::array<unsigned char, 32> hostID{};
    if (!g_settings.ensureHostID(hostID)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Fatal: failed to initialize host ID.");
        return 1;
    }

    std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
    std::string keyErrorMessage;
    if (!g_keyManager.GetNetworkKey(networkKey, &keyErrorMessage)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Fatal: failed to load network key before starting threads: %s", keyErrorMessage.c_str());
        return 1;
    }
    PrintNetworkKeyHash(networkKey);

    // Start worker threads
    if (StartClipboardNotification(OnClipboardNotification)) {
        if (StartMDNS(OnMDNSNotification)) {
            if (g_listener.Start()) {
                g_logger.log(__FUNCTION__, Logger::Level::Info, "Press Enter to exit...");
                std::cin.get();
                g_peerManager.ClearPeers();
                g_listener.Stop();
				g_logger.log(__FUNCTION__, Logger::Level::Info, "Listener stopped.");
            } else {
                g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to start TCP listener thread!");
            }
            StopMDNS();
			g_logger.log(__FUNCTION__, Logger::Level::Info, "mDNS stopped.");
        } else {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to start mDNS thread!");
        }
        StopClipboardNotification();
		g_logger.log(__FUNCTION__, Logger::Level::Info, "Clipboard notification stopped.");
    } else {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to start clipboard notification thread!");
    }

    g_peerManager.ClearPeers();
	g_logger.log(__FUNCTION__, Logger::Level::Info, "Peer manager cleared.");

    #ifdef _WIN32
        WSACleanup();
    #endif

    return 0;
}
