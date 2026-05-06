#pragma once

#include <array>
#include <cstdarg>
#include <atomic>
#include <future>
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
    void Terminate();
    bool IsRunning() const;
    std::array<unsigned char, 32> remoteHostID() const;
    std::wstring remoteHostName() const;

private:
    void ThreadProc();
    void log(const char* function, Logger::Level level, const wchar_t* message, ...) const;
    void logV(const char* function, Logger::Level level, const wchar_t* message, va_list args) const;

    ClipboardReceivedCallback clipboardReceivedCallback_;
    SOCKET socket_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{ false };
	std::atomic<bool> running_{ false };
    mutable std::mutex socketMutex_;
    mutable std::mutex remoteInfoMutex_;

    std::array<unsigned char, 32> remoteHostID_{};
    std::wstring remoteHostName_;
    std::wstring remoteIp_;
};
