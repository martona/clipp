#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "Listener.h"

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();

    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;

    bool Start();
    void Stop();
    bool Restart();

private:
    void ThreadProc();
    void OnClipboardReceived(const std::wstring& hostName, const HostId& hostID, ClipboardPayload& payload);

    static void OnMDNSNotification(const char* hostNameUtf8,
                                   const char* senderIp,
                                   const char* queryID,
                                   const char* nonce,
                                   const char* verb,
                                   unsigned short port,
                                   const HostId& remoteHostId);

    Listener listener_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable stopCV_;
    bool stopRequested_ = false;
    bool stopping_ = false;
};

extern NetworkRuntime g_networkRuntime;
