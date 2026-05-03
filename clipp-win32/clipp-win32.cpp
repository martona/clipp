// clipp-win32.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <sodium.h>
#include <mdns.h>

#include "ClipboardNotificationThread.h"
#include "MDNSThread.h"
#include <windows.h>

void OnClipboardNotification() {
    std::cout << "Clipboard debounced and processed!" << std::endl;
}

void OnMDNSNotification(const char* hostName) {
    std::cout << "mDNS notification received for host: " << hostName << std::endl;
}

int main() {
    // libsodium requires initialization before calling any other functions
    if (sodium_init() < 0) {
        std::cerr << "Fatal: libsodium failed to initialize!" << std::endl;
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

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
