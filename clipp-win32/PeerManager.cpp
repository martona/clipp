#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include "Peer.h"
#include "PeerManager.h"

PeerManager::PeerManager() {
};

PeerManager::~PeerManager() {
	ClearPeers();
};

void PeerManager::AddPeer(const wchar_t* hostName, const unsigned char* hostID, const char* ip, unsigned short port) {
	peers_.emplace_back(std::make_unique<Peer>(hostName, hostID, ip, port));
}

void PeerManager::RemovePeer(const unsigned char* hostID) {
	peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
		[hostID](const std::unique_ptr<Peer>& peer) {
			return memcmp(peer->hostID(), hostID, 32) == 0;
		}), peers_.end());
}

void PeerManager::ClearPeers() {
	peers_.clear();
}
