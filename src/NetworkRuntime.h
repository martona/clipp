#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
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
    void OnClipboardReceived(std::shared_ptr<const ClipboardPayload> payload);

    // Periodic work, driven by the runtime thread (~1s cadence): expire any deferred
    // peer removals, and re-announce discovery if the local interface addresses changed.
    void Tick(std::chrono::steady_clock::time_point now);
    void MaybeRepublishForNetworkChange(std::chrono::steady_clock::time_point now);

    static void OnDiscoveryEvent(MDNSDiscovery::Event event, const MDNSDiscovery::DiscoveredPeer& peer);

    Listener listener_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable stopCV_;
    bool stopRequested_ = false;
    bool stopping_ = false;

    // Touched only by the runtime thread (Stop() joins it before anyone else looks).
    uint64_t lastAddrHash_ = 0;
    std::chrono::steady_clock::time_point lastRepublish_{};
};

extern NetworkRuntime g_networkRuntime;
