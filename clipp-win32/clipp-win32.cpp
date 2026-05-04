// clipp-win32.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <conio.h>
#include <cstdlib>
#include <iostream>
#include <sodium.h>
#include <mdns.h>

#include <windows.h>

#include "Logger.h"
#include "ClipboardNotificationThread.h"
#include "KeyManager.h"
#include "MDNSThread.h"
#include "Listener.h"
#include "Peer.h"
#include "PeerManager.h"

Settings g_settings;
PeerManager g_peerManager;

KeyManager g_keyManager(g_settings);

void OnClientClipboardReceived(const wchar_t* hostName, const unsigned char* hostID, ClipboardPayload& payload);

namespace {
    Listener g_listener([](const std::wstring& hostName, const std::array<unsigned char, 32>& hostID, ClipboardPayload& payload) {
        OnClientClipboardReceived(hostName.c_str(), hostID.data(), payload);
    });
}

static std::string ReadHiddenLine(const std::string& prompt) {
    g_logger.log(__FUNCTION__, Logger::Level::Info, "%s", prompt.c_str());
    std::string input;
    // Get the standard input handle
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    // Save the current console mode
    GetConsoleMode(hStdin, &mode);
    // Disable the echo input flag
    SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
    // Read the input normally (backspaces are handled by the OS automatically)
    std::getline(std::cin, input);
    // Restore the original console mode
    SetConsoleMode(hStdin, mode);
    // Print a newline since the user's 'Enter' key press was also suppressed
    
    return input;
}

void OnClipboardNotification(HWND hwnd) {
    g_logger.log(__FUNCTION__, Logger::Level::Info, "Clipboard notification received");
    auto clipboardData = ReadClipboardData(hwnd);
    if (clipboardData.formatId == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Clipboard is empty or contains unsupported format");
        return;
	}
	auto payload = std::make_shared<const ClipboardPayload>(clipboardData);
	g_peerManager.BroadcastClipboard(payload);
	g_logger.log(__FUNCTION__, Logger::Level::Info, "Broadcasted clipboard data to peers (format ID: %u, size: %zu bytes)", clipboardData.formatId, clipboardData.rawData.size());
}

void OnClientClipboardReceived(const wchar_t* hostName, const unsigned char* hostID, ClipboardPayload& payload) {
    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Received clipboard data from client %s (format ID: %u, size: %zu bytes)", hostName, payload.formatId, payload.rawData.size());
	SetClipboardData(payload);
}

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

    if (std::string(verb) == "response" && rawHostID != nullptr) {
        int hostNameWLen = MultiByteToWideChar(CP_UTF8, 0, hostNameUtf8, -1, nullptr, 0);
        std::wstring hostNameW(hostNameWLen > 0 ? hostNameWLen - 1 : 0, L'\0');
        if (hostNameWLen > 1) {
            MultiByteToWideChar(CP_UTF8, 0, hostNameUtf8, -1, hostNameW.data(), hostNameWLen);
        }
        g_peerManager.AddPeer(hostNameW.c_str(), rawHostID, senderIp, port);
    }

    g_peerManager.CullPeers();
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "setkey") {
        std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
        const std::string keyInput = ReadHiddenLine("Enter 64-character network key hex: ");

        if (!g_keyManager.ParseHexNetworkKey(keyInput, networkKey)) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Invalid input. Expected exactly 64 hexadecimal characters.");
            return 1;
        }

        std::string errorMessage;
        if (!g_keyManager.SetNetworkKey(networkKey, &errorMessage)) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to store network key: %s", errorMessage.c_str());
            return 1;
        }

        g_logger.log(__FUNCTION__, Logger::Level::Info, "Network key saved successfully.");
        return 0;
    }

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

    return 0;
}
