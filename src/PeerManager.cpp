#include "Logger.h"
#include "PeerManager.h"
#include "PeerDisplay.h"
#include "HostId.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>

extern PeerDisplay g_peerDisplay;

PeerManager::PeerManager() {
}

PeerManager::~PeerManager() {
	ClearPeers();
}

void PeerManager::AddPeer(const wchar_t* hostName, const HostId& hostID, const wchar_t* ip, unsigned short port) {
	HostId incomingHostId = hostID;

	std::lock_guard<std::mutex> lock(peersMutex_);
	const auto found = std::find_if(peers_.begin(), peers_.end(), [&incomingHostId](const std::unique_ptr<Peer>& peer) {
		return (peer->hostID() == incomingHostId) && (peer->connType_ == Peer::ConnType::Outgoing);
	});
	if (found != peers_.end()) {
		g_logger.log(__FUNCTION__, Logger::Level::DDebug, L"PeerManager: peer already known; skipping duplicate.");
		return;
	}

	auto peer = std::make_unique<Peer>(hostName, &hostID, ip, port, nullptr,
		[](const HostId& hostID, uint64_t bytesSent, uint64_t bytesReceived) {
			g_peerDisplay.NotifyPeerBytes(hostID, bytesSent, bytesReceived);
		});
	g_peerDisplay.NotifyPeer(peer->hostName(), peer->hostID(), peer->connType_, peer->createdAt());
	Peer* peerPtr = peer.get();
	peers_.emplace_back(std::move(peer));
	peerPtr->Start();
	g_logger.log(__FUNCTION__, Logger::Level::Debug, L"PeerManager: added new peer (outgoing).");
}

void PeerManager::AddPeer(SOCKET socket, Peer::ClipboardReceivedCallback clipboardReceivedCallback) {
	auto peer = std::make_unique<Peer>(socket, std::move(clipboardReceivedCallback),
		[](const std::wstring& hostName, const HostId& hostID, Peer::ConnType connType, std::chrono::steady_clock::time_point connectedSince) {
			g_peerDisplay.NotifyPeer(hostName, hostID, connType, connectedSince);
		},
		[](const HostId& hostID, uint64_t bytesSent, uint64_t bytesReceived) {
			g_peerDisplay.NotifyPeerBytes(hostID, bytesSent, bytesReceived);
		});
	Peer* peerPtr = peer.get();
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		peers_.emplace_back(std::move(peer));
	}
	peerPtr->Start();
	g_logger.log(__FUNCTION__, Logger::Level::Debug, L"PeerManager: added new peer (incoming).");
}

void PeerManager::RemovePeer(const HostId& hostID) {
	std::lock_guard<std::mutex> lock(peersMutex_);
	peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
		[hostID](const std::unique_ptr<Peer>& peer) {
			if (peer->hostID() == hostID) {
				g_peerDisplay.NotifyPeerRemoved(peer->hostID(), peer->connType_);
				return true;
			} else {
				return false;
			}
		}), peers_.end());
}

void PeerManager::RemoveOutgoingPeer(const HostId& hostID) {
	std::lock_guard<std::mutex> lock(peersMutex_);
	peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
		[hostID](const std::unique_ptr<Peer>& peer) {
			if (peer->connType_ == Peer::ConnType::Outgoing && peer->hostID() == hostID) {
				peer->Stop();
				g_peerDisplay.NotifyPeerRemoved(peer->hostID(), peer->connType_);
				g_logger.log(__FUNCTION__, Logger::Level::Debug, L"PeerManager: removed outgoing peer (discovery reported gone).");
				return true;
			}
			return false;
		}), peers_.end());
}

void PeerManager::CullPeers() {
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(peersMutex_);
	peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [now](std::unique_ptr<Peer>& peer) {
		if (!peer->isRunning()) {
			g_peerDisplay.NotifyPeerRemoved(peer->hostID(), peer->connType_);
			g_logger.log(__FUNCTION__, Logger::Level::Debug, L"PeerManager: culled stopped peer.");
			return true;
		}
		const auto age = now - peer->createdAt();
		const auto silence = now - peer->lastPingReceivedAt();
		const bool dead = age >= std::chrono::minutes(1) && silence >= std::chrono::minutes(1);
		if (dead) {
			peer->Stop();
			g_peerDisplay.NotifyPeerRemoved(peer->hostID(), peer->connType_);
			g_logger.log(__FUNCTION__, Logger::Level::Debug, L"PeerManager: culled dead peer.");
		}
		return dead;
	}), peers_.end());
}

void PeerManager::ClearPeers() {
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (const auto& peer : peers_) {
		g_logger.log(__FUNCTION__, Logger::Level::Debug, L"PeerManager: clearing peer %ls", peer->hostName().c_str());
		peer->Stop();
		g_peerDisplay.NotifyPeerRemoved(peer->hostID(), peer->connType_);
	}
	peers_.clear();
}

void PeerManager::BroadcastClipboard(std::shared_ptr<const ClipboardPayload> payload) {
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (const auto& peer : peers_) {
		peer->PushMessage(payload);
	}
}

bool PeerManager::OnIncomingPeerEstablished() {
	std::lock_guard<std::mutex> lock(incomingCountMutex_);
	const bool isFirst = (establishedIncomingCount_ == 0);
	++establishedIncomingCount_;
	return isFirst;
}

void PeerManager::OnIncomingPeerLeft() {
	std::lock_guard<std::mutex> lock(incomingCountMutex_);
	if (establishedIncomingCount_ > 0) {
		--establishedIncomingCount_;
	}
}
