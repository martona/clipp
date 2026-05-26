#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ClipboardPayload.h"
#include "Listener.h"
#include "MDNSDiscovery.h"

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
    void OnClipboardReceived(const std::wstring& hostName, const HostId& hostID, std::shared_ptr<const ClipboardPayload> payload);

    static void OnDiscoveryEvent(MDNSDiscovery::Event event, const MDNSDiscovery::DiscoveredPeer& peer);

    Listener listener_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable stopCV_;
    bool stopRequested_ = false;
    bool stopping_ = false;
};

extern NetworkRuntime g_networkRuntime;
