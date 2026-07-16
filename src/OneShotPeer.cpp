#include "OneShotPeer.h"

#include <cstring>

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

bool PeerMatchesHost(const MDNSDiscovery::DiscoveredPeer& peer, const std::string& filter) {
    if (peer.ip == filter) return true;
    if (peer.deviceName.size() != filter.size()) return false;
    for (size_t i = 0; i < filter.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(peer.deviceName[i]);
        unsigned char b = static_cast<unsigned char>(filter[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + 32);
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + 32);
        if (a != b) return false;
    }
    return true;
}

std::optional<MDNSDiscovery::DiscoveredPeer> RelayPayloads(
    std::vector<ClipboardPayload> payloads,
    const HostId& localHostId,
    const std::string& localHostName,
    bool includeSelf,
    const std::string& hostFilter,
    bool* outFilterMatched) {
    for (ClipboardPayload& payload : payloads) {
        payload.meta.flags |= NetworkDefs::CLPM_FLAG_RELAY;
    }

    std::optional<MDNSDiscovery::DiscoveredPeer> via;
    MDNSDiscovery::BrowseStream(kBrowseCeiling, includeSelf,
        [&](const MDNSDiscovery::DiscoveredPeer& peer) -> bool {
            if (!hostFilter.empty() && !PeerMatchesHost(peer, hostFilter)) {
                return true;  // --host: skip everything but the named device
            }
            if (outFilterMatched != nullptr) {
                *outFilterMatched = true;  // a peer passed the filter; failure now means unreachable
            }
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
            // Delivery fence. A bare close here is abortive: the gateway pushes RSYN
            // (and sometimes SYNC) at every fresh incoming peer, we never read it, and
            // closing with unread inbound data turns the close into an RST — which can
            // destroy CLIP frames still sitting unread in the gateway's receive buffer.
            // That is the same race that deterministically dropped the fire-and-forget
            // REGW writes (see RunRegisterCopy's ack). The gateway's recv loop is
            // strictly serial, so a PONG proves every frame sent before the PING was
            // consumed and dispatched — an ack that needs no new protocol and works
            // against every deployed daemon. Draining the crosstalk while waiting also
            // makes the eventual close a clean FIN.
            if (!connection.SendFrame("PING")) {
                return true;  // try the next peer
            }
            bool acked = false;
            std::vector<unsigned char> frame;
            while (connection.RecvFrame(frame)) {
                if (frame.size() < 4) {
                    break;
                }
                if (std::memcmp(frame.data(), "PONG", 4) == 0) {
                    acked = true;
                    break;
                }
                if (std::memcmp(frame.data(), "SYNC", 4) == 0) {
                    // Activity-stream sync request meant for a long-lived daemon; we
                    // have nothing to replay, so end it rather than leave the gateway
                    // expecting a stream.
                    if (!connection.SendFrame("EOSY")) {
                        break;
                    }
                    continue;
                }
                // Ignore RSYN/REGW/CLIP and any other crosstalk while waiting.
            }
            if (!acked) {
                // No proof of delivery (timeout or reset). Trying the next peer may
                // re-send an item the gateway did apply — harmless: receivers dedup
                // on eventGuid and already-current content.
                return true;
            }
            via = peer;
            return false;  // delivered to a gateway; stop browsing
        });
    return via;
}

}  // namespace OneShot
