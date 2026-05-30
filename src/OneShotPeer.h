#pragma once

#include "ClipboardPayload.h"
#include "CryptoChannel.h"
#include "HostId.h"
#include "MDNSDiscovery.h"
#include "utils_socket.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// A single-shot outbound peer connection: connect to one discovered peer, run the
// client handshake, verify identity, then either push a clipboard payload (`copy`)
// or do a half-duplex request/response frame exchange (`paste`). Outbound-only —
// no background receive loop, no reconnect, no ping. One connection, one job, torn
// down on destruction. Shared by the desktop CLI and the iOS share extension.
//
// A watchdog thread enforces an overall session deadline: if the exchange stalls,
// it trips stopRequested_ and wakes the socket so the blocking send/recv helpers
// (utils_socket.h) bail out instead of parking forever.
class OneShotPeer {
public:
    OneShotPeer() = default;
    ~OneShotPeer();

    OneShotPeer(const OneShotPeer&) = delete;
    OneShotPeer& operator=(const OneShotPeer&) = delete;

    // Connect, handshake, and verify the remote identity against expectedHostId.
    // connectTimeout bounds the TCP connect; sessionDeadline bounds everything after
    // it (handshake + all subsequent Send/Recv). Returns false on any failure; the
    // object is then unusable (destroy it). One-shot: call Connect at most once.
    bool Connect(const std::string& ip, uint16_t port,
                 const HostId& localHostId, const std::string& localHostNameUtf8,
                 const HostId& expectedHostId,
                 std::chrono::milliseconds connectTimeout,
                 std::chrono::milliseconds sessionDeadline);

    const HostId& RemoteHostId() const { return remoteHostId_; }
    const CryptoChannel::Caps& RemoteCaps() const { return channel_.RemoteCaps(); }

    // copy: encode + send one CLIP frame.
    bool SendClipboardPayload(const ClipboardPayload& payload);

    // paste: send a request frame (e.g. "RCNT") and read response frames. On a
    // successful RecvFrame, out[0..4) is the tag and the remainder is the body.
    bool SendFrame(const char* tag4, const unsigned char* body = nullptr, uint32_t bodySize = 0);
    bool RecvFrame(std::vector<unsigned char>& out);

private:
    void Teardown();

    SOCKET socket_{ INVALID_SOCKET };
    SocketWakeEvent wakeEvent_;
    CryptoChannel channel_;
    HostId remoteHostId_{};
    std::atomic<bool> stopRequested_{ false };
    bool connected_{ false };

    // Watchdog (session deadline). finished_ signals a clean teardown so the thread
    // exits its wait without tripping stopRequested_.
    std::thread deadlineThread_;
    std::mutex deadlineMutex_;
    std::condition_variable deadlineCV_;
    bool finished_{ false };
};

namespace OneShot {

// Shared one-shot timeouts: TCP connect, the whole post-connect session, and the
// give-up ceiling for discovery (hit only when no peer answers).
constexpr auto kConnectTimeout = std::chrono::seconds(3);
constexpr auto kSessionTimeout = std::chrono::seconds(30);
constexpr auto kBrowseCeiling  = std::chrono::milliseconds(1200);

// Finds a gateway peer (the first that accepts) and pushes `payloads` to it with
// CLPM_FLAG_RELAY set, so that peer rebroadcasts them to the synced mesh — one push
// instead of a fan-out. Returns the peer it relayed through, or nullopt if none
// accepted within kBrowseCeiling. Shared by the CLI `copy` verb and the iOS share
// extension. `includeSelf` surfaces this device's own GUI (CLI uses true; the share
// extension uses false, since it can't assume its own app is running to relay).
std::optional<MDNSDiscovery::DiscoveredPeer> RelayPayloads(
    std::vector<ClipboardPayload> payloads,
    const HostId& localHostId,
    const std::string& localHostName,
    bool includeSelf);

}  // namespace OneShot
