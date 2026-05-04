#include "PeerManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>

PeerManager::PeerManager() {
}

PeerManager::~PeerManager() {
	ClearPeers();
}

void PeerManager::AddPeer(const wchar_t* hostName, const unsigned char* hostID, const char* ip, unsigned short port) {
	std::array<unsigned char, 32> incomingHostId{};
	std::memcpy(incomingHostId.data(), hostID, incomingHostId.size());

	std::lock_guard<std::mutex> lock(peersMutex_);
	const auto found = std::find_if(peers_.begin(), peers_.end(), [&incomingHostId](const std::unique_ptr<Peer>& peer) {
		return peer->hostID() == incomingHostId;
	});
	if (found != peers_.end()) {
		std::wcout << L"PeerManager: peer already known; skipping duplicate." << std::endl;
		return;
	}

	auto peer = std::make_unique<Peer>(hostName, hostID, ip, port);
	peer->Start();
	peers_.emplace_back(std::move(peer));
	std::wcout << L"PeerManager: added new peer." << std::endl;
}

void PeerManager::RemovePeer(const unsigned char* hostID) {
	std::lock_guard<std::mutex> lock(peersMutex_);
	peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
		[hostID](const std::unique_ptr<Peer>& peer) {
			const auto peerHostID = peer->hostID();
			return std::memcmp(peerHostID.data(), hostID, peerHostID.size()) == 0;
		}), peers_.end());
}

void PeerManager::CullPeers() {
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(peersMutex_);
	peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [now](std::unique_ptr<Peer>& peer) {
		const auto age = now - peer->createdAt();
		const auto silence = now - peer->lastPingReceivedAt();
		const bool dead = age >= std::chrono::minutes(1) && silence >= std::chrono::minutes(1);
		if (dead) {
			peer->Stop();
			std::wcout << L"PeerManager: culled dead peer." << std::endl;
		}
		return dead;
	}), peers_.end());
}

void PeerManager::ClearPeers() {
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (const auto& peer : peers_) {
		std::wcout << L"PeerManager: clearing peer " << peer->hostName() << std::endl;
		peer->Stop();
	}
	peers_.clear();
}
