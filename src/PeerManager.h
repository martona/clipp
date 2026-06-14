#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "Peer.h"

class PeerManager {
public:
	PeerManager();
	~PeerManager();
	void AddPeer(const wchar_t* hostName, const HostId& hostID, const wchar_t* ip, unsigned short port);
	void AddPeer(SOCKET socket, Peer::ClipboardReceivedCallback clipboardReceivedCallback);
	void RemovePeer(const HostId& hostID);
	// Tears down only the outgoing connection for this hostId, leaving any inbound connection
	// (e.g., from an iOS share extension running alongside the same hostId) intact.
	void RemoveOutgoingPeer(const HostId& hostID);
	void CullPeers();
	void ClearPeers();
	void BroadcastClipboard(std::shared_ptr<const ClipboardPayload> payload);
	// Send a pre-encoded register frame to every connected peer advertising
	// CAP0_SERVES_REGISTERS — rebroadcasts a re-stamped relay write to the mesh.
	void BroadcastRegisterFrame(const std::array<char, 4>& tag, const std::vector<unsigned char>& body);

	// Tracks how many incoming peers have completed their handshake.
	// OnIncomingPeerEstablished returns true exactly on the 0→1 transition —
	// the activity-stream sync trigger. Every call to OnIncomingPeerEstablished
	// must be paired with one to OnIncomingPeerLeft when the peer goes away.
	bool OnIncomingPeerEstablished();
	void OnIncomingPeerLeft();
private:
	mutable std::mutex peersMutex_;
	std::vector<std::unique_ptr<Peer>> peers_;
	std::mutex incomingCountMutex_;
	std::size_t establishedIncomingCount_{ 0 };
};
