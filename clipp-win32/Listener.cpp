#include "Listener.h"

#include <chrono>
#include <iostream>
#include <memory>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "Client.h"
#include "Settings.h"

#pragma comment(lib, "ws2_32.lib")

Listener::Listener() {}
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
    {
        std::lock_guard<std::mutex> lock(listenSocketMutex_);
        if (listenSocket_ != INVALID_SOCKET) {
            shutdown(listenSocket_, SD_BOTH);
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    clientManager_.Terminate();
    std::wcout << L"Listener stopped." << std::endl;
}

void Listener::ThreadProc() {
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::wcerr << L"Listener: WSAStartup failed." << std::endl;
        running_.store(false);
        return;
    }

    while (running_.load()) {
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            std::wcerr << L"Listener: socket creation failed; retrying." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(listenSocketMutex_);
            listenSocket_ = listenSock;
        }

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(static_cast<u_short>(g_settings.tcpPort()));
        if (inet_pton(AF_INET, g_settings.listenerIp().c_str(), &bindAddr.sin_addr) != 1) {
            std::wcerr << L"Listener: invalid listener IP; retrying." << std::endl;
            closesocket(listenSock);
            {
                std::lock_guard<std::mutex> lock(listenSocketMutex_);
                if (listenSocket_ == listenSock) {
                    listenSocket_ = INVALID_SOCKET;
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        if (bind(listenSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
            std::wcerr << L"Listener: bind failed; retrying." << std::endl;
            closesocket(listenSock);
            {
                std::lock_guard<std::mutex> lock(listenSocketMutex_);
                if (listenSocket_ == listenSock) {
                    listenSocket_ = INVALID_SOCKET;
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
            std::wcerr << L"Listener: listen failed; retrying." << std::endl;
            closesocket(listenSock);
            {
                std::lock_guard<std::mutex> lock(listenSocketMutex_);
                if (listenSocket_ == listenSock) {
                    listenSocket_ = INVALID_SOCKET;
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        std::wcout << L"Listener running on " << g_settings.listenerIp().c_str() << L":" << g_settings.tcpPort() << std::endl;

        while (running_.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSock, &readSet);
            timeval timeout{};
            timeout.tv_sec = 2;
            const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
            clientManager_.Cleanup();

            if (!running_.load()) {
                break;
            }

            if (ready == SOCKET_ERROR) {
                std::wcerr << L"Listener: select failed; recreating listener socket." << std::endl;
                break;
            }
            if (ready == 0) {
                continue;
            }

            SOCKET clientSock = accept(listenSock, nullptr, nullptr);
            if (clientSock == INVALID_SOCKET) {
                std::wcerr << L"Listener: accept failed." << std::endl;
                continue;
            }

            clientManager_.AddClient(std::make_unique<Client>(clientSock));
            std::wcout << L"Accepted incoming TCP client." << std::endl;
        }

        closesocket(listenSock);
        {
            std::lock_guard<std::mutex> lock(listenSocketMutex_);
            if (listenSocket_ == listenSock) {
                listenSocket_ = INVALID_SOCKET;
            }
        }
    }

    WSACleanup();
}
