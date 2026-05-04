#pragma once

#include <array>
#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <winsock2.h>
#include <functional>

#include "ClipboardData.h"

class Client {
public:
    using ClipboardReceivedCallback = std::function<void(const std::wstring&, const std::array<unsigned char, 32>&, ClipboardPayload&)>;

    explicit Client(SOCKET socket, ClipboardReceivedCallback clipboardReceivedCallback);
    ~Client();

    void Start();
    void Terminate();
    bool IsRunning() const;
    std::array<unsigned char, 32> remoteHostID() const;
    std::wstring remoteHostName() const;

private:
    void ThreadProc();
    static bool RecvAll(SOCKET sock, char* buffer, int length);
    static bool SendAll(SOCKET sock, const char* buffer, int length);

    ClipboardReceivedCallback clipboardReceivedCallback_;
    SOCKET socket_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{ false };
	std::atomic<bool> running_{ false };
    mutable std::mutex socketMutex_;
    mutable std::mutex remoteInfoMutex_;

    std::array<unsigned char, 32> remoteHostID_{};
    std::wstring remoteHostName_;
};
