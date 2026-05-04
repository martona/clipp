// clipp-win32.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <conio.h>
#include <cstdlib>
#include <iostream>
#include <sodium.h>
#include <mdns.h>

#include <windows.h>

#include "ClipboardNotificationThread.h"
#include "KeyManager.h"
#include "MDNSThread.h"
#include "Listener.h"
#include "Peer.h"
#include "PeerManager.h"

Settings g_settings;
PeerManager g_peerManager;

namespace {
    KeyManager g_keyManager(g_settings);
    Listener g_listener;
}

static std::string ReadHiddenLine(const std::string& prompt) {
    std::cout << prompt;
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
    std::cout << std::endl;
    return input;
}

void OnClipboardNotification() {
    std::cout << "Clipboard debounced and processed!" << std::endl;
}

void OnMDNSNotification(const wchar_t* hostName, const wchar_t* hostID, const wchar_t* senderIp, const wchar_t* queryID, const wchar_t* nonce, const wchar_t* verb, u_short port, const unsigned char* rawHostID) {
	std::wcout << L"mDNS notification received for host: " << hostName << L" / " << hostID
		<< L"\n  from: " << senderIp << L":" << port
               << L"\n  verb:    " << verb
               << L"\n  queryID: " << queryID
               << L"\n  nonce:   " << nonce << std::endl;

    if (std::wstring(verb) == L"response" && rawHostID != nullptr) {
        std::wstring senderIpW(senderIp);
        std::string senderIpA;
        if (!senderIpW.empty()) {
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, senderIpW.c_str(), (int)senderIpW.size(), nullptr, 0, nullptr, nullptr);
            senderIpA.resize(size_needed);
            WideCharToMultiByte(CP_UTF8, 0, senderIpW.c_str(), (int)senderIpW.size(), &senderIpA[0], size_needed, nullptr, nullptr);
        }
        g_peerManager.AddPeer(hostName, rawHostID, senderIpA.c_str(), port);
    }

    g_peerManager.CullPeers();
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "setkey") {
        std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
        const std::string keyInput = ReadHiddenLine("Enter 64-character network key hex: ");

        if (!g_keyManager.ParseHexNetworkKey(keyInput, networkKey)) {
            std::cerr << "Invalid input. Expected exactly 64 hexadecimal characters." << std::endl;
            return 1;
        }

        std::string errorMessage;
        if (!g_keyManager.SetNetworkKey(networkKey, &errorMessage)) {
            std::cerr << "Failed to store network key: " << errorMessage << std::endl;
            return 1;
        }

        std::cout << "Network key saved successfully." << std::endl;
        return 0;
    }

    // libsodium requires initialization before calling any other functions
    if (sodium_init() < 0) {
        std::cerr << "Fatal: libsodium failed to initialize!" << std::endl;
        return 1;
    }
    std::cout << "libsodium initialized successfully." << std::endl;

    std::array<unsigned char, 32> hostID{};
    if (!g_settings.ensureHostID(hostID)) {
        std::cerr << "Fatal: failed to initialize host ID." << std::endl;
        return 1;
    }

    std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
    std::string keyErrorMessage;
    if (!g_keyManager.GetNetworkKey(networkKey, &keyErrorMessage)) {
        std::cerr << "Fatal: failed to load network key before starting threads: " << keyErrorMessage << std::endl;
        return 1;
    }

    // Start worker threads
    if (StartClipboardNotification(OnClipboardNotification)) {
        if (StartMDNS(OnMDNSNotification)) {
            if (g_listener.Start()) {
                std::cout << "Press Enter to exit..." << std::endl;
                std::cin.get();
                g_listener.Stop();
				std::cout << "Listener stopped." << std::endl;
            } else {
                std::cerr << "Failed to start TCP listener thread!" << std::endl;
            }
            StopMDNS();
			std::cout << "mDNS stopped." << std::endl;
        } else {
            std::cerr << "Failed to start mDNS thread!" << std::endl;
        }
        StopClipboardNotification();
		std::cout << "Clipboard notification stopped." << std::endl;
    } else {
        std::cerr << "Failed to start clipboard notification thread!" << std::endl;
    }

    g_peerManager.ClearPeers();
	std::cout << "Peer manager cleared." << std::endl;

    return 0;
}
