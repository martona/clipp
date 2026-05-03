// clipp-win32.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <conio.h>
#include <cstdlib>
#include <iostream>
#include <sodium.h>
#include <mdns.h>

#include "ClipboardNotificationThread.h"
#include "KeyManager.h"
#include "MDNSThread.h"
#include <windows.h>

Settings g_settings;

namespace {
KeyManager g_keyManager(g_settings);

bool ParseHexNetworkKey(const std::string& hex, std::array<unsigned char, KeyManager::NetworkKeySize>& networkKey) {
    if (hex.size() != KeyManager::NetworkKeySize * 2) {
        return false;
    }

    for (size_t i = 0; i < KeyManager::NetworkKeySize; ++i) {
        const std::string byteHex = hex.substr(i * 2, 2);
        char* endPtr = nullptr;
        const long value = std::strtol(byteHex.c_str(), &endPtr, 16);
        if (endPtr == nullptr || *endPtr != '\0' || value < 0 || value > 255) {
            return false;
        }
        networkKey[i] = static_cast<unsigned char>(value);
    }
    return true;
}

std::string ReadHiddenLine(const std::string& prompt) {
    std::cout << prompt;
    std::string input;

    while (true) {
        int ch = _getch();
        if (ch == '\r' || ch == '\n') {
            std::cout << std::endl;
            break;
        }
        if (ch == '\b') {
            if (!input.empty()) {
                input.pop_back();
            }
            continue;
        }
        if (ch == 0 || ch == 224) {
            (void)_getch();
            continue;
        }
        input.push_back(static_cast<char>(ch));
    }

    return input;
}
} // namespace

void OnClipboardNotification() {
    std::cout << "Clipboard debounced and processed!" << std::endl;
}

void OnMDNSNotification(const wchar_t* hostName, const wchar_t* senderIp, const wchar_t* queryID, const wchar_t* nonce, const wchar_t* verb, u_short port) {
    std::wcout << L"mDNS notification received for host: " << hostName
               << L" from IP: " << senderIp
		       << L" on port: " << port
               << L"\n  verb:    " << verb
               << L"\n  queryID: " << queryID
               << L"\n  nonce:   " << nonce << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "setkey") {
        std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
        const std::string keyInput = ReadHiddenLine("Enter 64-character network key hex: ");

        if (!ParseHexNetworkKey(keyInput, networkKey)) {
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

    std::array<unsigned char, KeyManager::NetworkKeySize> networkKey{};
    std::string keyErrorMessage;
    if (!g_keyManager.GetNetworkKey(networkKey, &keyErrorMessage)) {
        std::cerr << "Fatal: failed to load network key before starting threads: " << keyErrorMessage << std::endl;
        return 1;
    }

    std::cout << "libsodium initialized successfully." << std::endl;

    // Let's generate some random bytes just to prove the library is linked and active
    char buffer[32];
    randombytes_buf(buffer, sizeof(buffer));

    std::cout << "Generated 32 random bytes." << std::endl;

    // A simple mdns string creation to test the linker
    mdns_string_t test_string = { "test", 4 };
    std::cout << "mDNS string created with length: " << test_string.length << std::endl;

    if (StartClipboardNotification(OnClipboardNotification)) {
        if (StartMDNS(OnMDNSNotification)) {
            std::cout << "Press Enter to exit..." << std::endl;
            std::cin.get();
            StopMDNS();
        } else {
            std::cerr << "Failed to start mDNS thread!" << std::endl;
        }
        StopClipboardNotification();
    } else {
        std::cerr << "Failed to start clipboard notification thread!" << std::endl;
    }
    return 0;
}
