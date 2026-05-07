#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include "platform.h"
#include "PeerManager.h"

class Listener {
public:
    using ClipboardReceivedCallback = Peer::ClipboardReceivedCallback;

    Listener(ClipboardReceivedCallback clipboardReceivedCallback = nullptr);
    ~Listener();

    bool Start();
    void Stop();

private:
    void ThreadProc();
	void InterruptibleSleep(std::chrono::milliseconds duration);

    std::atomic<bool> running_{ false };
    std::mutex stopMutex_;
    std::condition_variable stopCV_;
    std::thread thread_;
    SOCKET listenSocket_{ INVALID_SOCKET };
    std::mutex listenSocketMutex_;
	ClipboardReceivedCallback clipboardReceivedCallback_{};
};
