#pragma once

#include <array>
#include <cstdarg>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <functional>

#include "platform.h"

#include "ClipboardData.h"
#include "Logger.h"

class Client {
public:
    using ClipboardReceivedCallback = std::function<void(const std::wstring&, const std::array<unsigned char, 32>&, ClipboardPayload&)>;

    explicit Client(SOCKET socket, ClipboardReceivedCallback clipboardReceivedCallback);
    ~Client();

    void Start();
    void Stop();
    bool isRunning() const;

    std::wstring hostName() const;
    std::array<unsigned char, 32> hostID() const;
    std::wstring ip() const;
    unsigned short port() const;
    std::chrono::steady_clock::time_point lastPingReceivedAt() const;
    std::chrono::steady_clock::time_point createdAt() const;


private:
    void ThreadProc();
    void CloseSocket();
    void log(const char* function, Logger::Level level, const wchar_t* message, ...) const;
    void logV(const char* function, Logger::Level level, const wchar_t* message, va_list args) const;

    ClipboardReceivedCallback clipboardReceivedCallback_;

    mutable std::mutex dataMutex_;
    std::wstring hostName_;
    std::wstring ip_;
    unsigned short port_{};
    std::array<unsigned char, 32> hostID_{};
    std::chrono::steady_clock::time_point createdAt_;
    std::chrono::steady_clock::time_point lastPingReceivedAt_;

    std::thread thread_;
    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> running_{ false };

    SOCKET socket_{ INVALID_SOCKET };
};
