#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <winsock2.h>

#include "ClientManager.h"

class Listener {
public:
    Listener();
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
    ClientManager clientManager_;
    SOCKET listenSocket_{ INVALID_SOCKET };
    std::mutex listenSocketMutex_;
};
