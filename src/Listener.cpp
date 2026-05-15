#include "Logger.h"
#include "Listener.h"

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include "PeerManager.h"
#include "Settings.h"
#include "KeyManager.h"

extern PeerManager g_peerManager;
extern KeyManager g_keyManager;

static bool HasNetworkKey() {
    std::array<unsigned char, KeyManager::NetworkKeySize> handshakeKey{};
    std::string errorMessage;
    return g_keyManager.GetKey(KeyManager::KeyRole::TcpHandshakeClientToServer, handshakeKey, &errorMessage);
}

Listener::Listener(ClipboardReceivedCallback clipboardReceivedCallback) { 
	clipboardReceivedCallback_ = std::move(clipboardReceivedCallback);
}

Listener::~Listener() { Stop(); }


bool Listener::Start() {
    if (running_.exchange(true)) {
        return false;
    }
    thread_ = std::thread(&Listener::ThreadProc, this);
    return true;
}

void Listener::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    stopCV_.notify_all();

    SOCKET listenSock = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(listenSocketMutex_);
        if (listenSocket_ != INVALID_SOCKET) {
            listenSock = listenSocket_;
            listenSocket_ = INVALID_SOCKET;
        }
    }
    if (listenSock != INVALID_SOCKET) {
        shutdown(listenSock, SD_BOTH);
        closesocket(listenSock);
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Listener stopped.");
}

void Listener::InterruptibleSleep(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(stopMutex_);
    stopCV_.wait_for(lock, duration, [this]() { return !running_.load(); });
}

void Listener::CloseListenSocket(SOCKET listenSock) {
    bool shouldClose = false;
    {
        std::lock_guard<std::mutex> lock(listenSocketMutex_);
        if (listenSocket_ == listenSock) {
            listenSocket_ = INVALID_SOCKET;
            shouldClose = true;
        }
    }

    if (shouldClose) {
        closesocket(listenSock);
    }
}

void Listener::ThreadProc() {
    while (running_.load()) {
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Listener: socket creation failed; retrying.");
			InterruptibleSleep(std::chrono::seconds(5));
            continue;
        }

        int reuseAddr = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

        {
            std::lock_guard<std::mutex> lock(listenSocketMutex_);
            listenSocket_ = listenSock;
        }

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(static_cast<u_short>(g_settings.tcpPort()));
        if (inet_pton(AF_INET, g_settings.listenerIp().c_str(), &bindAddr.sin_addr) != 1) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Listener: invalid listener IP; retrying.");
            CloseListenSocket(listenSock);
            InterruptibleSleep(std::chrono::seconds(5));
            continue;
        }

        if (bind(listenSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Listener: bind failed; retrying.");
            CloseListenSocket(listenSock);
            InterruptibleSleep(std::chrono::seconds(5));
            continue;
        }

        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Listener: listen failed; retrying.");
            CloseListenSocket(listenSock);
            InterruptibleSleep(std::chrono::seconds(5));
            continue;
        }

        g_logger.log(__FUNCTION__, Logger::Level::Info, L"Listener running on %hs:%hu", g_settings.listenerIp().c_str(), g_settings.tcpPort());

        while (running_.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSock, &readSet);
            timeval timeout{};
            timeout.tv_sec = 2;
            const int ready = select(static_cast<int>(listenSock) + 1, &readSet, nullptr, nullptr, &timeout);

            if (!running_.load()) {
                break;
            }

            if (ready == SOCKET_ERROR) {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Listener: select failed; recreating listener socket.");
                break;
            }
            if (ready == 0) {
                continue;
            }

            SOCKET clientSock = accept(listenSock, nullptr, nullptr);
            if (clientSock == INVALID_SOCKET) {
                g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Listener: accept failed.");
                continue;
            }

            if (!HasNetworkKey()) {
                closesocket(clientSock);
                g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Listener: closed incoming TCP client because no network key is configured.");
                continue;
            }

			g_peerManager.AddPeer(clientSock, clipboardReceivedCallback_);
            g_logger.log(__FUNCTION__, Logger::Level::Info, L"Accepted incoming TCP client.");
        }

        CloseListenSocket(listenSock);
    }
}
