#include "Client.h"

#include <iostream>
#include <cwchar>
#include <chrono>
#include <cstring>
#include <ws2tcpip.h>

namespace {
#pragma pack(push, 1)
struct ClientHello {
    wchar_t selector[8];
    unsigned short version;
    unsigned char hostID[32];
    wchar_t hostName[256];
};
#pragma pack(pop)

constexpr const wchar_t* kSelector = L"clipp";
constexpr unsigned short kVersion = 1;
}

Client::Client(SOCKET socket) {
	socket_ = socket;
}

Client::~Client() {
    Terminate();
}

void Client::Start() {
    running_.store(true);
    thread_ = std::thread(&Client::ThreadProc, this);
}

void Client::Terminate() {
    stopRequested_.store(true);
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        if (socket_ != INVALID_SOCKET) {
            shutdown(socket_, SD_BOTH);
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Client::IsRunning() const {
	return running_.load();
}

bool Client::RecvAll(SOCKET sock, char* buffer, int length) {
    int total = 0;
    while (total < length) {
        const int received = recv(sock, buffer + total, length - total, 0);
        if (received <= 0) {
            return false;
        }
        total += received;
    }
    return true;
}

bool Client::SendAll(SOCKET sock, const char* buffer, int length) {
    int total = 0;
    while (total < length) {
        const int sent = send(sock, buffer + total, length - total, 0);
        if (sent <= 0) {
            return false;
        }
        total += sent;
    }
    return true;
}

void Client::ThreadProc() {
    ClientHello hello{};
    if (!RecvAll(socket_, reinterpret_cast<char*>(&hello), sizeof(hello))) {
        std::wcerr << L"Client handshake read failed." << std::endl;
    } else if (std::wcsncmp(hello.selector, kSelector, std::wcslen(kSelector)) != 0 || ntohs(hello.version) != kVersion) {
        std::wcerr << L"Client handshake validation failed." << std::endl;
    } else {
        std::memcpy(remoteHostID_.data(), hello.hostID, sizeof(hello.hostID));
        hello.hostName[_countof(hello.hostName) - 1] = L'\0';
        remoteHostName_ = hello.hostName;
        std::wcout << L"Client connected: " << remoteHostName_ << std::endl;

        char packet[4] = {};
        while (!stopRequested_.load()) {
            if (!RecvAll(socket_, packet, sizeof(packet))) {
                break;
            }

            if (std::memcmp(packet, "PING", 4) == 0) {
                if (!SendAll(socket_, "PONG", 4)) {
                    break;
                }
            }
        }
    }

    running_.store(false);
    std::wcout << L"Client thread exiting." << std::endl;
}
