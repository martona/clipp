#include "NetworkRuntime.h"

#include "Logger.h"
#include "MDNSDiscovery.h"
#include "PeerManager.h"
#include "Settings.h"
#include "utils.h"

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
#define CLIPP_IOS_CLIPBOARD_RECEIVE_STUB 1
void CLPIOSReceiveClipboardPayload(std::shared_ptr<const ClipboardPayload> payload);
#else
#include "ClipboardActivityStore.h"
#include "Clipboard.h"
#define CLIPP_IOS_CLIPBOARD_RECEIVE_STUB 0
#endif

extern PeerManager g_peerManager;
extern Settings g_settings;
#if !CLIPP_IOS_CLIPBOARD_RECEIVE_STUB
extern ClipboardActivityStore g_clipboardActivityStore;
#endif

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

    {
        std::unique_lock<std::mutex> lock(mutex_);
        stopCV_.wait(lock, [this]() { return stopRequested_; });
    }

    if (listenerStarted) {
        listener_.Stop();
    }

    if (mdnsStarted) {
        MDNSDiscovery::Stop();
        g_logger.log(__FUNCTION__, Logger::Level::Info, "DNS-SD discovery stopped.");
    }

    g_peerManager.ClearPeers();
    g_logger.log(__FUNCTION__, Logger::Level::Info, "Peer manager cleared.");
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
    }
#endif
}

void NetworkRuntime::OnDiscoveryEvent(MDNSDiscovery::Event event, const MDNSDiscovery::DiscoveredPeer& peer) {
    if (event == MDNSDiscovery::Event::Removed) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            "DNS-SD: peer removed (%s).", peer.deviceName.c_str());
        g_peerManager.RemoveOutgoingPeer(peer.hostId);
        return;
    }

    // Added.
    g_logger.log(__FUNCTION__, Logger::Level::Debug,
        "DNS-SD: peer added '%s' / %s at %s:%hu (osType=%u).",
        peer.deviceName.c_str(),
        peer.hostId.ToHexString().c_str(),
        peer.ip.c_str(),
        peer.port,
        static_cast<unsigned>(peer.osType));

    if (peer.port == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::DDebug, "DNS-SD: peer advertises no TCP port; skipping.");
        return;
    }

    const std::wstring hostNameW = Utf8ToWideString(peer.deviceName);
    const std::wstring ipW = Utf8ToWideString(peer.ip);
    g_peerManager.AddPeer(hostNameW.c_str(), peer.hostId, ipW.c_str(), peer.port);
}
