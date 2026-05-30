#include "OneShotPeer.h"

#include "ClipboardWire.h"
#include "Logger.h"

OneShotPeer::~OneShotPeer() {
    Teardown();
}

bool OneShotPeer::Connect(const std::string& ip, uint16_t port,
                          const HostId& localHostId, const std::string& localHostNameUtf8,
                          const HostId& expectedHostId,
                          std::chrono::milliseconds connectTimeout,
                          std::chrono::milliseconds sessionDeadline) {
    socket_ = ConnectTcpSocket(ip, port, connectTimeout);
    if (socket_ == INVALID_SOCKET) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"OneShotPeer: connect to %hs:%hu failed.", ip.c_str(), port);
        return false;
    }

    if (!wakeEvent_.Initialize()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"OneShotPeer: failed to init wake socket.");
        return false;  // destructor closes socket_
    }

    // Watchdog covers handshake + the caller's whole exchange.
    deadlineThread_ = std::thread([this, sessionDeadline]() {
        std::unique_lock<std::mutex> lock(deadlineMutex_);
        if (!deadlineCV_.wait_for(lock, sessionDeadline, [this]() { return finished_; })) {
            stopRequested_.store(true);
            wakeEvent_.Signal();
        }
    });

    SocketIoContext io{ socket_, wakeEvent_, stopRequested_ };
    std::string remoteHostNameUtf8;
    if (!channel_.ClientHandshake(io, localHostId, localHostNameUtf8, remoteHostId_, remoteHostNameUtf8)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"OneShotPeer: handshake with %hs:%hu failed.", ip.c_str(), port);
        return false;
    }

    if (remoteHostId_ != expectedHostId) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"OneShotPeer: identity mismatch for %hs:%hu.", ip.c_str(), port);
        return false;
    }

    connected_ = true;
    return true;
}

bool OneShotPeer::SendClipboardPayload(const ClipboardPayload& payload) {
    if (!connected_) return false;
    SocketIoContext io{ socket_, wakeEvent_, stopRequested_ };
    return ClipboardWire::SendClipboardPayload(channel_, io, payload);
}

bool OneShotPeer::SendFrame(const char* tag4, const unsigned char* body, uint32_t bodySize) {
    if (!connected_) return false;
    SocketIoContext io{ socket_, wakeEvent_, stopRequested_ };
    return channel_.SendFrame(io, tag4, body, bodySize);
}

bool OneShotPeer::RecvFrame(std::vector<unsigned char>& out) {
    if (!connected_) return false;
    SocketIoContext io{ socket_, wakeEvent_, stopRequested_ };
    return channel_.RecvFrame(io, out);
}

void OneShotPeer::Teardown() {
    {
        std::lock_guard<std::mutex> lock(deadlineMutex_);
        finished_ = true;
    }
    deadlineCV_.notify_one();
    if (deadlineThread_.joinable()) {
        deadlineThread_.join();
    }
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    wakeEvent_.Close();
}

namespace OneShot {

std::optional<MDNSDiscovery::DiscoveredPeer> RelayPayloads(
    std::vector<ClipboardPayload> payloads,
    const HostId& localHostId,
    const std::string& localHostName,
    bool includeSelf) {
    for (ClipboardPayload& payload : payloads) {
        payload.meta.flags |= NetworkDefs::CLPM_FLAG_RELAY;
    }

    std::optional<MDNSDiscovery::DiscoveredPeer> via;
    MDNSDiscovery::BrowseStream(kBrowseCeiling, includeSelf,
        [&](const MDNSDiscovery::DiscoveredPeer& peer) -> bool {
            OneShotPeer connection;
            if (!connection.Connect(peer.ip, peer.port, localHostId, localHostName, peer.hostId,
                                    kConnectTimeout, kSessionTimeout)) {
                return true;  // unreachable; try the next peer
            }
            // Gate on CAP0_SERVES_RECENT even though that cap names the paste/RCNT path,
            // not relaying. The connection: a peer advertises it only if it's a current
            // build, and a current build ships BOTH the RCNT responder and the relay
            // responder together — so the bit is a reliable proxy for "this peer will
            // rebroadcast a RELAY item to the mesh." Without the check, an older peer
            // (which accepts the frame but silently ignores the RELAY flag) would look
            // like success and strand the item on one device. Skipping to a current-build
            // peer means its rebroadcast still reaches any older peers in the mesh anyway.
            if ((connection.RemoteCaps()[0] & CryptoChannel::CAP0_SERVES_RECENT) == 0) {
                return true;  // pre-relay peer; try the next
            }
            for (const ClipboardPayload& payload : payloads) {
                if (!connection.SendClipboardPayload(payload)) {
                    return true;  // send failed mid-stream; try the next peer
                }
            }
            via = peer;
            return false;  // delivered to a gateway; stop browsing
        });
    return via;
}

}  // namespace OneShot
