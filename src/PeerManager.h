#pragma once

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
private:
	mutable std::mutex peersMutex_;
	std::vector<std::unique_ptr<Peer>> peers_;
};
