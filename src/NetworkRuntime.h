#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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

    // Event-driven runtime work. The thread sleeps until woken; nothing polls.
    //  - SignalWake: wake the thread (from the network-change monitor, the reconciler's
    //    deferred-removal path, or Stop). `interfacesChanged` means "re-hash interfaces".
    //  - ComputeNextWake: the soonest deadline the thread must wake for (pending republish
    //    cooldown, or a deferred peer removal); nullopt = sleep until explicitly woken.
    //  - ProcessWake: re-hash if flagged, republish if pending+cooled-down, sweep removals.
    void SignalWake(bool interfacesChanged);
    std::optional<std::chrono::steady_clock::time_point> ComputeNextWake();
    void ProcessWake(std::chrono::steady_clock::time_point now);

    static void OnDiscoveryEvent(MDNSDiscovery::Event event, const MDNSDiscovery::DiscoveredPeer& peer);

    Listener listener_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable stopCV_;
    bool stopRequested_ = false;
    bool stopping_ = false;

    // Wake coordination (guarded by mutex_). wakePending_ = "re-evaluate now";
    // interfacesChanged_ = "the network-change monitor fired, re-hash".
    bool wakePending_ = false;
    bool interfacesChanged_ = false;

    // Touched only by the runtime thread (Stop() joins it before anyone else looks).
    uint64_t lastAddrHash_ = 0;       // fingerprint last acted on
    uint64_t pendingHash_ = 0;        // latest fingerprint awaiting a (cooled-down) republish
    bool republishPending_ = false;
    std::chrono::steady_clock::time_point lastRepublish_{};
};

extern NetworkRuntime g_networkRuntime;
