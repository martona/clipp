#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "Peer.h"

class PeerManager {
public:
	PeerManager();
	~PeerManager();
	void AddPeer(const wchar_t* hostName, const unsigned char* hostID, const char* ip, unsigned short port);
	void RemovePeer(const unsigned char* hostID);
	void CullPeers();
	void ClearPeers();
private:
	mutable std::mutex peersMutex_;
	std::vector<std::unique_ptr<Peer>> peers_;
};
