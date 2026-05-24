#include "NetworkRuntime.h"

#include "Logger.h"
#include "MDNSThread.h"
#include "PeerManager.h"
#include "Settings.h"
#include "utils.h"

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
#define CLIPP_IOS_CLIPBOARD_RECEIVE_STUB 1
void CLPIOSReceiveClipboardPayload(const std::wstring& hostName, const ClipboardPayload& payload);
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
    : listener_([this](const std::wstring& hostName, const HostId& hostID, ClipboardPayload& payload) {
        OnClipboardReceived(hostName, hostID, payload);
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

    if (StartMDNS(&NetworkRuntime::OnMDNSNotification)) {
        mdnsStarted = true;
    } else {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to start mDNS thread!");
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
        StopMDNS();
        g_logger.log(__FUNCTION__, Logger::Level::Info, "mDNS stopped.");
    }

    g_peerManager.ClearPeers();
    g_logger.log(__FUNCTION__, Logger::Level::Info, "Peer manager cleared.");
}

void NetworkRuntime::OnClipboardReceived(const std::wstring& hostName, const HostId&, ClipboardPayload& payload) {
    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Received clipboard data from client %ls (format: %ls, ID: %u, size: %zu bytes)",
        hostName.c_str(),
        ClippClipboardFormatNameW(payload.formatId),
        payload.formatId,
        payload.rawData.size());
#if CLIPP_IOS_CLIPBOARD_RECEIVE_STUB
    CLPIOSReceiveClipboardPayload(hostName, payload);
#else
    if (IsClipboardDataCurrent(payload)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring clipboard data from client %ls because the same contents are already current.", hostName.c_str());
        return;
    }

    const uint64_t itemID = g_clipboardActivityStore.AddIncoming(hostName, payload);
    SetClipboardData(payload, true, g_clipboardActivityStore.PayloadReference(itemID));
#endif
}

void NetworkRuntime::OnMDNSNotification(const char* hostNameUtf8,
                                        const char* senderIp,
                                        const char* queryID,
                                        const char* nonce,
                                        const char* verb,
                                        unsigned short port,
                                        const HostId& remoteHostId)
{
    HostId ourHostId;
    if (!g_settings.getHostID(ourHostId)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Failed to get host ID from settings during mDNS notification handling.");
        return;
    }

    g_logger.log(__FUNCTION__, Logger::Level::DDebug,
        "mDNS notification received for host: %s / %s\n  from: %s:%hu\n  verb:    %s\n  queryID: %s\n  nonce:   %s",
        hostNameUtf8, remoteHostId.ToHexString().c_str(), senderIp, port, verb, queryID, nonce);

    if (ourHostId == remoteHostId) {
        g_logger.log(__FUNCTION__, Logger::Level::DDebug, "mDNS notification is from self; ignoring");
        return;
    }

    size_t hostNameWLen = utf8_to_utf16(hostNameUtf8, strlen(hostNameUtf8), nullptr, 0);
    std::wstring hostNameW(hostNameWLen, L'\0');
    if (hostNameWLen > 0) {
        utf8_to_utf16(hostNameUtf8, strlen(hostNameUtf8), hostNameW.data(), hostNameW.size());
    }
    g_peerManager.AddPeer(hostNameW.c_str(), remoteHostId, Utf8ToWideString(senderIp).c_str(), port);
}
