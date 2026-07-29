#include "Logger.h"
#include "PeerManager.h"
#include "PeerDisplay.h"
#include "HostId.h"
#include "PeerLimits.h"
#include "utils.h"

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

PeerManager::OutgoingPeerAdmission PeerManager::AddPeer(
	const wchar_t* hostName,
	const HostId& hostID,
	const wchar_t* ip,
	unsigned short port) {
	HostId incomingHostId = hostID;

	std::lock_guard<std::mutex> lock(peersMutex_);
	const auto found = std::find_if(peers_.begin(), peers_.end(), [&incomingHostId](const std::unique_ptr<Peer>& peer) {
		return (peer->hostID() == incomingHostId) && (peer->connType_ == Peer::ConnType::Outgoing);
	});
	if (found != peers_.end()) {
		g_logger.log(__FUNCTION__, Logger::Level::DDebug, L"PeerManager: peer already known; skipping duplicate.");
		return OutgoingPeerAdmission::AlreadyPresent;
	}

	const std::size_t outgoingCount = static_cast<std::size_t>(std::count_if(
		peers_.begin(), peers_.end(), [](const std::unique_ptr<Peer>& peer) {
			return peer->connType_ == Peer::ConnType::Outgoing;
		}));
	if (!PeerLimits::CanAddOutgoing(outgoingCount)) {
		if (const auto report = outgoingPeerLog_.RecordRejection()) {
			if (report->firstReport) {
				g_logger.log(__FUNCTION__, Logger::Level::Warning,
					L"Defensive limit tripped: outgoing peers; rejected=1, outgoing=%zu/%zu.",
					outgoingCount, PeerLimits::MaxOutgoingPeers);
			}
			else {
				g_logger.log(__FUNCTION__, Logger::Level::Warning,
					L"Defensive limit still active: outgoing peers; rejected_since_last_report=%llu, outgoing=%zu/%zu.",
					static_cast<unsigned long long>(report->rejectedSinceLastReport),
					outgoingCount, PeerLimits::MaxOutgoingPeers);
			}
		}
		return OutgoingPeerAdmission::RejectedPeerLimit;
	}

	auto peer = std::make_unique<Peer>(hostName, &hostID, ip, port, nullptr,
		[](const HostId& hostID, uint64_t bytesSent, uint64_t bytesReceived) {
			g_peerDisplay.NotifyPeerBytes(hostID, bytesSent, bytesReceived);
		});
	g_peerDisplay.NotifyPeer(peer->hostName(), peer->hostID(), peer->osType(), peer->connType_, peer->createdAt());
	Peer* peerPtr = peer.get();
	peers_.emplace_back(std::move(peer));
	peerPtr->Start();
	g_logger.log(__FUNCTION__, Logger::Level::Debug, L"PeerManager: added new peer (outgoing).");
	return OutgoingPeerAdmission::Accepted;
}

PeerManager::IncomingPeerAdmission PeerManager::AddIncomingPeer(
	SOCKET socket,
	Peer::ClipboardReceivedCallback clipboardReceivedCallback) {
	const std::string peerIpUtf8 = SocketPeerIp(socket);
	if (peerIpUtf8.empty()) {
		closesocket(socket);
		return IncomingPeerAdmission::RejectedMissingAddress;
	}
	const std::wstring peerIp = Utf8ToWideString(peerIpUtf8);

	std::lock_guard<std::mutex> lock(peersMutex_);
	std::size_t pendingGlobal = 0;
	std::size_t pendingForIp = 0;
	for (const auto& existingPeer : peers_) {
		if (!existingPeer->isIncomingAuthenticationPending()) {
			continue;
		}
		++pendingGlobal;
		if (existingPeer->ip() == peerIp) {
			++pendingForIp;
		}
	}

	const auto pendingAdmission =
		PeerLimits::EvaluatePendingAuthentication(pendingGlobal, pendingForIp);
	if (pendingAdmission == PeerLimits::PendingAuthenticationAdmission::RejectedGlobalLimit) {
		closesocket(socket);
		if (const auto report = globalPendingAuthLog_.RecordRejection()) {
			if (report->firstReport) {
				g_logger.log(__FUNCTION__, Logger::Level::Warning,
					L"Defensive limit tripped: pending authentication (global); rejected=1, pending=%zu/%zu.",
					pendingGlobal, PeerLimits::MaxPendingIncomingAuthentications);
			}
			else {
				g_logger.log(__FUNCTION__, Logger::Level::Warning,
					L"Defensive limit still active: pending authentication (global); rejected_since_last_report=%llu, pending=%zu/%zu.",
					static_cast<unsigned long long>(report->rejectedSinceLastReport),
					pendingGlobal, PeerLimits::MaxPendingIncomingAuthentications);
			}
		}
		return IncomingPeerAdmission::RejectedGlobalAuthLimit;
	}
	if (pendingAdmission == PeerLimits::PendingAuthenticationAdmission::RejectedPerIpLimit) {
		closesocket(socket);
		if (const auto report = perIpPendingAuthLog_.RecordRejection()) {
			if (report->firstReport) {
				g_logger.log(__FUNCTION__, Logger::Level::Warning,
					L"Defensive limit tripped: pending authentication (per IP); rejected=1, latest_source=%ls, pending_for_source=%zu/%zu, pending_global=%zu/%zu.",
					peerIp.c_str(),
					pendingForIp, PeerLimits::MaxPendingIncomingAuthenticationsPerIp,
					pendingGlobal, PeerLimits::MaxPendingIncomingAuthentications);
			}
			else {
				g_logger.log(__FUNCTION__, Logger::Level::Warning,
					L"Defensive limit still active: pending authentication (per IP); rejected_since_last_report=%llu, latest_source=%ls, pending_for_source=%zu/%zu, pending_global=%zu/%zu.",
					static_cast<unsigned long long>(report->rejectedSinceLastReport),
					peerIp.c_str(),
					pendingForIp, PeerLimits::MaxPendingIncomingAuthenticationsPerIp,
					pendingGlobal, PeerLimits::MaxPendingIncomingAuthentications);
			}
		}
		return IncomingPeerAdmission::RejectedPerIpAuthLimit;
	}

	auto peer = std::make_unique<Peer>(socket, std::move(clipboardReceivedCallback),
		[](const std::wstring& hostName, const HostId& hostID, OsType osType, Peer::ConnType connType, std::chrono::steady_clock::time_point connectedSince) {
			g_peerDisplay.NotifyPeer(hostName, hostID, osType, connType, connectedSince);
		},
		[](const HostId& hostID, uint64_t bytesSent, uint64_t bytesReceived) {
			g_peerDisplay.NotifyPeerBytes(hostID, bytesSent, bytesReceived);
		});
	Peer* peerPtr = peer.get();
	peers_.emplace_back(std::move(peer));
	// Start while peersMutex_ is still held so the pending-authentication reservation
	// becomes visible atomically with admission. The worker may finish immediately, but
	// asynchronous culling simply waits for this short critical section to end.
	peerPtr->Start();
	g_logger.log(__FUNCTION__, Logger::Level::Debug, L"PeerManager: added new peer (incoming).");
	return IncomingPeerAdmission::Accepted;
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
		// Only incoming peers are silence-culled. Outgoing peers self-manage: they retry
		// forever (lastPingReceivedAt_ freezes while disconnected, so silence is meaningless
		// for them) and are removed only when discovery reports the peer gone, via the
		// OutgoingReconciler -> RemoveOutgoingPeer. Silence-culling a live, reconnecting
		// outgoing peer would reap a healthy connection and desync the reconciler.
		const bool dead = peer->connType_ == Peer::ConnType::Incoming
			&& age >= std::chrono::minutes(1) && silence >= std::chrono::minutes(1);
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

size_t PeerManager::BroadcastClipboard(std::shared_ptr<const ClipboardPayload> payload) {
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (const auto& peer : peers_) {
		peer->PushMessage(payload);
	}
	return peers_.size();
}

void PeerManager::BroadcastRegisterFrame(const std::array<char, 4>& tag, const std::vector<unsigned char>& body) {
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (const auto& peer : peers_) {
		if (peer->RemoteServesRegisters()) {
			peer->PushRawFrame(tag, body);  // copies the body per recipient
		}
	}
}

void PeerManager::BroadcastFrame(const std::array<char, 4>& tag, const std::vector<unsigned char>& body) {
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (const auto& peer : peers_) {
		peer->PushRawFrame(tag, body);  // copies the body per recipient
	}
}

PeerManager::IncomingPeerEstablishment PeerManager::TryEstablishIncomingPeer() {
	std::lock_guard<std::mutex> lock(incomingCountMutex_);
	if (!PeerLimits::CanEstablishIncoming(establishedIncomingCount_)) {
		if (const auto report = establishedIncomingLog_.RecordRejection()) {
			if (report->firstReport) {
				g_logger.log(__FUNCTION__, Logger::Level::Warning,
					L"Defensive limit tripped: authenticated incoming peers; rejected=1, incoming=%zu/%zu.",
					establishedIncomingCount_, PeerLimits::MaxAuthenticatedIncomingPeers);
			}
			else {
				g_logger.log(__FUNCTION__, Logger::Level::Warning,
					L"Defensive limit still active: authenticated incoming peers; rejected_since_last_report=%llu, incoming=%zu/%zu.",
					static_cast<unsigned long long>(report->rejectedSinceLastReport),
					establishedIncomingCount_, PeerLimits::MaxAuthenticatedIncomingPeers);
			}
		}
		return IncomingPeerEstablishment::RejectedPeerLimit;
	}
	const bool isFirst = (establishedIncomingCount_ == 0);
	++establishedIncomingCount_;
	return isFirst
		? IncomingPeerEstablishment::AcceptedFirst
		: IncomingPeerEstablishment::Accepted;
}

void PeerManager::OnIncomingPeerLeft() {
	std::lock_guard<std::mutex> lock(incomingCountMutex_);
	if (establishedIncomingCount_ > 0) {
		--establishedIncomingCount_;
	}
}
