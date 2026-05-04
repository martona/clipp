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

    std::atomic<bool> running_{ false };
    std::thread thread_;
    ClientManager clientManager_;
    SOCKET listenSocket_{ INVALID_SOCKET };
    std::mutex listenSocketMutex_;
};
