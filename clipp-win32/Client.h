#pragma once

#include <array>
#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <winsock2.h>

class Client {
public:
    explicit Client(SOCKET socket);
    ~Client();

    void Start();
    void Terminate();
    bool IsRunning() const;

private:
    void ThreadProc();
    static bool RecvAll(SOCKET sock, char* buffer, int length);
    static bool SendAll(SOCKET sock, const char* buffer, int length);

    SOCKET socket_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{ false };
	std::atomic<bool> running_{ false };
    mutable std::mutex socketMutex_;

    std::array<unsigned char, 32> remoteHostID_{};
    std::wstring remoteHostName_;
};
