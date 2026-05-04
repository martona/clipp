#include "Logger.h"
#include "Client.h"

#include <iostream>
#include <cwchar>
#include <chrono>
#include <cstring>
#include <ws2tcpip.h>

#include "CryptoChannel.h"

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

std::array<unsigned char, 32> Client::remoteHostID() const {
    std::lock_guard<std::mutex> lock(remoteInfoMutex_);
    return remoteHostID_;
}

std::wstring Client::remoteHostName() const {
    std::lock_guard<std::mutex> lock(remoteInfoMutex_);
    return remoteHostName_;
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
    CryptoChannel channel;
    std::array<unsigned char, 32> remoteHostId{};
    std::string remoteHostNameUtf8;
    if (!channel.ServerHandshake(socket_, remoteHostId, remoteHostNameUtf8)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, L"Client secure handshake failed.");
    } else {
        {
            std::lock_guard<std::mutex> lock(remoteInfoMutex_);
            remoteHostID_ = remoteHostId;
            int remoteHostNameWLen = MultiByteToWideChar(CP_UTF8, 0, remoteHostNameUtf8.c_str(), -1, nullptr, 0);
            std::wstring remoteHostName(remoteHostNameWLen > 0 ? remoteHostNameWLen - 1 : 0, L'\0');
            if (remoteHostNameWLen > 1) {
                MultiByteToWideChar(CP_UTF8, 0, remoteHostNameUtf8.c_str(), -1, remoteHostName.data(), remoteHostNameWLen);
            }
            remoteHostName_ = remoteHostName;
        }
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Client connected: %s", remoteHostNameUtf8.c_str());

        char packet[4] = {};
        while (!stopRequested_.load()) {
            if (!channel.RecvTaggedMessage(socket_, packet)) {
                break;
            }

            if (std::memcmp(packet, "PING", 4) == 0) {
                g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Client: PING");
                if (!channel.SendTaggedMessage(socket_, "PONG")) {
                    break;
                }
            }
        }
    }

    running_.store(false);

}
