#include "NetworkRuntime.h"

#include <chrono>
#include <functional>
#include <map>
#include <optional>

#include "HostId.h"
#include "Logger.h"
#include "MDNSDiscovery.h"
#include "NetworkChangeMonitor.h"
#include "NetworkInterfaces.h"
#include "PeerManager.h"
#include "Settings.h"
#include "utils.h"

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
#define CLIPP_IOS_CLIPBOARD_RECEIVE_STUB 1
void CLPIOSReceiveClipboardPayload(std::shared_ptr<const ClipboardPayload> payload);
#else
#include "ClipboardActions.h"
#include "ClipboardActivityStore.h"
#include "Clipboard.h"
#include "ClipboardFlowUi.h"
#include "ClipboardFormat.h"
#include "RegisterStore.h"
#define CLIPP_IOS_CLIPBOARD_RECEIVE_STUB 0
#endif

extern PeerManager g_peerManager;
extern Settings g_settings;
#if !CLIPP_IOS_CLIPBOARD_RECEIVE_STUB
extern ClipboardActivityStore g_clipboardActivityStore;
#endif

namespace {

// ============================================================================
// Outgoing-peer reconciler
// ============================================================================
// Sits between discovery's (already hostId-collapsed) Added/Removed events and
// PeerManager, so that a re-announce -- ours after an interface change, a peer's app
// restart, or just the unfavorable mDNS goodbye-before-announce ordering -- doesn't
// needlessly tear down and rebuild a healthy outgoing connection.
//
//  * Removed is DEFERRED by a short grace window rather than acted on immediately. If a
//    same-hostId Added arrives within the window (the new instance resolving just after
//    the old instance's goodbye), the pending removal is cancelled.
//  * On that Added: same endpoint (ip:port) -> pure republish, swallow it and keep the
//    live connection. Different endpoint -> re-point: drop the stale connection and
//    reconnect to the new address (PeerManager::AddPeer alone can't update it -- it
//    dedups outgoing peers by hostId).
//  * A genuine departure (no replacement within grace) fires the real removal when the
//    runtime thread sweeps expired entries.
//
// The favorable race (the new instance resolves before the old goodbye) never reaches
// here: discovery's liveByHostId collapse already swallows both events.
constexpr auto kRemovalGrace = std::chrono::seconds(4);

// Lower bound between successive re-announcements; coalesces interface flapping (DHCP,
// link-local settling, IPv6 privacy-address rotation) into at most one republish/window.
constexpr auto kMinRepublishInterval = std::chrono::seconds(10);

class OutgoingReconciler {
public:
    void OnEvent(MDNSDiscovery::Event event, const MDNSDiscovery::DiscoveredPeer& peer) {
        // PeerManager mutations run under our lock so `active_` can't disagree with
        // PeerManager when two resolves for the same hostId race (Win32 delivers resolve
        // callbacks on concurrent threads). AddPeer/RemoveOutgoingPeer never reach back
        // into the reconciler, so the lock order is strictly one-way.
        std::function<void()> wakeRuntime;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (event == MDNSDiscovery::Event::Removed) {
                // Defer; Sweep() makes it real if no replacement shows up within the grace.
                if (active_.count(peer.hostId)) {
                    pendingRemoval_[peer.hostId] = std::chrono::steady_clock::now() + kRemovalGrace;
                    g_logger.log("Reconciler", Logger::Level::Debug,
                        "Peer '%s' departure reported; deferring removal up to %llds for a re-announce.",
                        peer.deviceName.c_str(),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kRemovalGrace).count()));
                    // Wake the runtime thread so it re-arms its sleep to this grace deadline.
                    wakeRuntime = wake_;
                }
            } else {
                // Added: a (re)appearance cancels any pending removal for this hostId.
                pendingRemoval_.erase(peer.hostId);
                auto it = active_.find(peer.hostId);
                if (it == active_.end()) {
                    active_.emplace(peer.hostId, peer);
                    g_logger.log("Reconciler", Logger::Level::Debug,
                        "Peer '%s' discovered at %s:%hu; connecting.",
                        peer.deviceName.c_str(), peer.ip.c_str(), peer.port);
                    ForwardAddPeer(peer);
                } else if (it->second.ip != peer.ip || it->second.port != peer.port) {
                    // Endpoint moved: drop the stale connection and reconnect to the new
                    // address (AddPeer alone dedups outgoing peers by hostId, can't update).
                    g_logger.log("Reconciler", Logger::Level::Debug,
                        "Peer '%s' endpoint changed %s:%hu -> %s:%hu; reconnecting.",
                        peer.deviceName.c_str(), it->second.ip.c_str(), it->second.port,
                        peer.ip.c_str(), peer.port);
                    it->second = peer;
                    g_peerManager.RemoveOutgoingPeer(peer.hostId);
                    ForwardAddPeer(peer);
                } else {
                    // Pure republish (same hostId + endpoint): the whole point of the
                    // reconciler is to NOT churn a healthy connection here.
                    g_logger.log("Reconciler", Logger::Level::Debug,
                        "Peer '%s' re-announced at unchanged endpoint %s:%hu; keeping live connection.",
                        peer.deviceName.c_str(), peer.ip.c_str(), peer.port);
                }
            }
        }
        if (wakeRuntime) wakeRuntime();
    }

    void Sweep(std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pendingRemoval_.begin(); it != pendingRemoval_.end();) {
            if (now >= it->second) {
                const HostId hostId = it->first;
                std::string deviceName;
                auto activeIt = active_.find(hostId);
                if (activeIt != active_.end()) deviceName = activeIt->second.deviceName;
                active_.erase(hostId);
                it = pendingRemoval_.erase(it);
                g_logger.log("Reconciler", Logger::Level::Debug,
                    "Peer '%s' removal grace expired with no re-announce; dropping.",
                    deviceName.c_str());
                g_peerManager.RemoveOutgoingPeer(hostId);
            } else {
                ++it;
            }
        }
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.clear();
        pendingRemoval_.clear();
        wake_ = nullptr;
    }

    // Set by NetworkRuntime: invoked (outside our lock) when a removal is deferred, so the
    // runtime thread can re-arm its sleep to the new grace deadline.
    void SetWakeCallback(std::function<void()> wake) {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_ = std::move(wake);
    }

    // Soonest deferred-removal deadline, or nullopt if none pending.
    std::optional<std::chrono::steady_clock::time_point> NextRemovalDeadline() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::optional<std::chrono::steady_clock::time_point> nearest;
        for (const auto& entry : pendingRemoval_) {
            if (!nearest || entry.second < *nearest) nearest = entry.second;
        }
        return nearest;
    }

private:
    static void ForwardAddPeer(const MDNSDiscovery::DiscoveredPeer& peer) {
        const std::wstring hostNameW = Utf8ToWideString(peer.deviceName);
        const std::wstring ipW = Utf8ToWideString(peer.ip);
        g_peerManager.AddPeer(hostNameW.c_str(), peer.hostId, ipW.c_str(), peer.port);
    }

    std::mutex mutex_;
    // Logical peers handed to PeerManager (one per hostId), with their last endpoint.
    std::map<HostId, MDNSDiscovery::DiscoveredPeer> active_;
    // hostId -> deadline after which a deferred Removed becomes real.
    std::map<HostId, std::chrono::steady_clock::time_point> pendingRemoval_;
    // Wakes the runtime thread when a removal is deferred (set by NetworkRuntime).
    std::function<void()> wake_;
};

OutgoingReconciler& Reconciler() {
    static OutgoingReconciler instance;
    return instance;
}

}  // namespace

NetworkRuntime::NetworkRuntime()
    : listener_([this](std::shared_ptr<const ClipboardPayload> payload) {
        OnClipboardReceived(std::move(payload));
    }) {
}

NetworkRuntime::~NetworkRuntime() {
    Stop();
}

bool NetworkRuntime::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable() || stopping_) {
        return false;
    }

    stopRequested_ = false;
    thread_ = std::thread(&NetworkRuntime::ThreadProc, this);
    return true;
}

void NetworkRuntime::Stop() {
    std::thread threadToJoin;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_.joinable()) {
            return;
        }

        stopRequested_ = true;
        stopping_ = true;
        threadToJoin = std::move(thread_);
    }

    stopCV_.notify_all();

    if (threadToJoin.joinable()) {
        threadToJoin.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
    }
}

bool NetworkRuntime::Restart() {
    Stop();
    return Start();
}

void NetworkRuntime::ThreadProc() {
    Reconciler().Reset();  // no carryover from a previous run
    Reconciler().SetWakeCallback([this]() { SignalWake(/*interfacesChanged=*/false); });

    bool mdnsStarted = false;
    bool listenerStarted = false;

    if (MDNSDiscovery::Start(&NetworkRuntime::OnDiscoveryEvent)) {
        mdnsStarted = true;
    } else {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to start DNS-SD discovery!");
    }

    if (listener_.Start()) {
        listenerStarted = true;
    } else {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to start TCP listener thread!");
    }

    if (mdnsStarted || listenerStarted) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Network runtime started.");
    }

    // Baseline the interface fingerprint so we only re-announce on *subsequent* changes,
    // and arm the cooldown clock so nothing republishes during startup settling.
    lastAddrHash_ = clipp::HashLocalInterfaceAddresses();
    pendingHash_ = lastAddrHash_;
    republishPending_ = false;
    lastRepublish_ = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wakePending_ = false;
        interfacesChanged_ = false;
    }

    // Event-driven interface-change watch: the OS signals us instead of us polling.
    clipp::NetworkChangeMonitor* monitor =
        clipp::StartNetworkChangeMonitor([this]() { SignalWake(/*interfacesChanged=*/true); });
    if (monitor) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            "Network change monitor armed (event-driven); interface fingerprint baseline %016llx.",
            static_cast<unsigned long long>(lastAddrHash_));
    } else {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            "Network change monitor unavailable; interface changes will not auto re-announce.");
    }

    // Sleep until something happens: a monitor event, the soonest pending deadline (a
    // republish cooldown or a deferred peer removal), or stop. No polling when idle.
    for (;;) {
        const std::optional<std::chrono::steady_clock::time_point> deadline = ComputeNextWake();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!stopRequested_ && !wakePending_) {
                if (deadline) {
                    stopCV_.wait_until(lock, *deadline, [this]() { return stopRequested_ || wakePending_; });
                } else {
                    stopCV_.wait(lock, [this]() { return stopRequested_ || wakePending_; });
                }
            }
            if (stopRequested_) break;
            wakePending_ = false;
        }
        ProcessWake(std::chrono::steady_clock::now());
    }

    if (monitor) {
        clipp::StopNetworkChangeMonitor(monitor);  // blocks until no callback is in flight
    }

    if (listenerStarted) {
        listener_.Stop();
    }

    if (mdnsStarted) {
        MDNSDiscovery::Stop();
        g_logger.log(__FUNCTION__, Logger::Level::Info, "DNS-SD discovery stopped.");
    }

    g_peerManager.ClearPeers();
    Reconciler().Reset();
    g_logger.log(__FUNCTION__, Logger::Level::Info, "Peer manager cleared.");
}

void NetworkRuntime::SignalWake(bool interfacesChanged) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (interfacesChanged) interfacesChanged_ = true;
        wakePending_ = true;
    }
    stopCV_.notify_one();
}

std::optional<std::chrono::steady_clock::time_point> NetworkRuntime::ComputeNextWake() {
    // republishPending_/lastRepublish_ are runtime-thread-only and this runs on that
    // thread, so no lock; NextRemovalDeadline() locks the reconciler internally.
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (republishPending_) {
        deadline = lastRepublish_ + kMinRepublishInterval;
    }
    if (auto removal = Reconciler().NextRemovalDeadline()) {
        if (!deadline || *removal < *deadline) deadline = *removal;
    }
    return deadline;
}

void NetworkRuntime::ProcessWake(std::chrono::steady_clock::time_point now) {
    bool rehash;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rehash = interfacesChanged_;
        interfacesChanged_ = false;
    }

    if (rehash) {
        // A monitor event is a coarse "re-check" hint; the hash decides if the address set
        // we advertise actually moved.
        const uint64_t hash = clipp::HashLocalInterfaceAddresses();
        if (hash != 0 && hash != lastAddrHash_) {
            republishPending_ = true;
            pendingHash_ = hash;
            g_logger.log("MaybeRepublishForNetworkChange", Logger::Level::DDebug,
                "Interface event: fingerprint %016llx -> %016llx; queuing re-announce.",
                static_cast<unsigned long long>(lastAddrHash_), static_cast<unsigned long long>(hash));
        } else if (hash != 0) {
            g_logger.log("MaybeRepublishForNetworkChange", Logger::Level::DDebug,
                "Interface event: no change to advertised address set (fingerprint %016llx).",
                static_cast<unsigned long long>(hash));
        }
        // hash == 0 -> enumeration failed transiently; leave state, retry on next event.
    }

    if (republishPending_) {
        const auto sinceLast = now - lastRepublish_;
        if (sinceLast >= kMinRepublishInterval) {
            g_logger.log("MaybeRepublishForNetworkChange", Logger::Level::Debug,
                "Interface address set changed (fingerprint %016llx -> %016llx); re-announcing discovery.",
                static_cast<unsigned long long>(lastAddrHash_), static_cast<unsigned long long>(pendingHash_));
            MDNSDiscovery::Republish();
            lastAddrHash_ = pendingHash_;
            lastRepublish_ = now;
            republishPending_ = false;
        } else {
            // Still cooling down; ComputeNextWake() will time us out at lastRepublish_ +
            // cooldown to fire it. DDebug -- this can recur across wakes within the window.
            g_logger.log("MaybeRepublishForNetworkChange", Logger::Level::DDebug,
                "Re-announce within cooldown (%lld of %lld ms elapsed); deferring.",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(sinceLast).count()),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(kMinRepublishInterval).count()));
        }
    }

    Reconciler().Sweep(now);
}

void NetworkRuntime::OnClipboardReceived(std::shared_ptr<const ClipboardPayload> payload) {
    const bool isReplay = (payload->meta.flags & NetworkDefs::CLPM_FLAG_SYNC_REPLAY) != 0;
    const bool isSourceMarkedPrivate = (payload->meta.flags & NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE) != 0;
    // Zero-length payload with the private flag is the sender's "marked private,
    // sync skipped" placeholder. The activity store should still get the entry
    // (so the user sees that something happened), but the OS clipboard must not
    // be touched and the hash guard should not apply (multiple placeholders in
    // a row would otherwise dedup).
    const bool isPrivatePlaceholder = isSourceMarkedPrivate && payload->EncodedBytes().empty();
    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Received clipboard data from %hs (format: %ls, ID: %u, encoded size: %zu bytes%ls%ls)",
        payload->meta.originHostName,
        ClippClipboardFormatNameW(payload->meta.formatId),
        payload->meta.formatId,
        payload->EncodedBytes().size(),
        isReplay ? L", sync replay" : L"",
        isPrivatePlaceholder ? L", private placeholder" : (isSourceMarkedPrivate ? L", source marked private" : L""));

    // Relay: a one-shot sender (CLI `copy` / iOS share ext) handed us a single item
    // to fan out to the mesh. Rebroadcast it with the relay bit cleared so recipients
    // apply-but-don't-relay (one hop; eventGuid dedup backstops loops). Done before
    // the local-apply below, and independent of IsClipboardDataCurrent, so a fresh
    // relay event propagates even if its content already sits on our own clipboard.
    if ((payload->meta.flags & NetworkDefs::CLPM_FLAG_RELAY) != 0 && !isReplay) {
        auto forwarded = std::make_shared<ClipboardPayload>();
        forwarded->meta = payload->meta;
        forwarded->meta.flags &= ~NetworkDefs::CLPM_FLAG_RELAY;
        forwarded->SetEncodedBytes(std::vector<unsigned char>(payload->EncodedBytes()));
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Relaying clipboard item to the mesh on behalf of %hs.",
            payload->meta.originHostName);
        g_peerManager.BroadcastClipboard(forwarded);
        payload = forwarded;  // apply/store the cleared version locally too
    }
#if CLIPP_IOS_CLIPBOARD_RECEIVE_STUB
    CLPIOSReceiveClipboardPayload(std::move(payload));
#else
    // Sync-replay events are historical and must NOT touch the OS clipboard or
    // the "current clipboard" hash guard — they represent past activity from
    // other devices, not the current paste-buffer state.
    if (!isReplay && !isPrivatePlaceholder && IsClipboardDataCurrent(*payload)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring clipboard data from %hs because the same contents are already current.", payload->meta.originHostName);
        return;
    }

    g_clipboardActivityStore.Add(payload);
    if (!isReplay && !isPrivatePlaceholder) {
        SetClipboardData(payload, true);
        // Ambient GUI feedback (tray / menu-bar nudge + last-event tooltip):
        // fire only on the path that actually replaced the OS clipboard.
        clipp::NotifyClipboardFlow(clipp::ClipboardFlowDirection::Received,
                                   payload->meta.originHostName);
        // Mirror the now-current clipboard into the default ("") register for `ls`.
        clipp::MirrorClipboardToDefaultRegister(payload);
    }
#endif
}

void NetworkRuntime::OnDiscoveryEvent(MDNSDiscovery::Event event, const MDNSDiscovery::DiscoveredPeer& peer) {
    if (event == MDNSDiscovery::Event::Removed) {
        g_logger.log(__FUNCTION__, Logger::Level::DDebug,
            "DNS-SD: raw removal event (%s).", peer.deviceName.c_str());
        Reconciler().OnEvent(event, peer);
        return;
    }

    // Added. This now fires on every successful resolve (not once per hostId), so the
    // raw event is DDebug; the reconciler logs the actual connect/repoint/keep decision.
    g_logger.log(__FUNCTION__, Logger::Level::DDebug,
        "DNS-SD: raw resolve event '%s' / %s at %s:%hu (osType=%u).",
        peer.deviceName.c_str(),
        peer.hostId.ToHexString().c_str(),
        peer.ip.c_str(),
        peer.port,
        static_cast<unsigned>(peer.osType));

    if (peer.port == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::DDebug, "DNS-SD: peer advertises no TCP port; skipping.");
        return;
    }

    Reconciler().OnEvent(event, peer);
}
