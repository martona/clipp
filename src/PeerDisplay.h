#pragma once

#include <array>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Peer.h"
#include "HostId.h"

// Outgoing connection state, as seen from this device. (Incoming state is just
// "we have an inbound socket or not" — the two-way nature is captured by the pair.)
enum class PeerConnState {
	Connecting,   // outgoing TCP attempt in progress
	Connected,    // outgoing TCP + handshake complete
	Backoff,      // outgoing failed, retrying after a delay
	Failed,       // outgoing gave up (rare; usually we keep retrying)
};

struct PeerDisplayItem {
	std::wstring hostName;
	HostId hostID;
	bool hasIncomingConnection{ false };
	bool hasOutgoingConnection{ false };
	PeerConnState outgoingConnState{ PeerConnState::Connecting };
	uint64_t bytesSent{};
	uint64_t bytesReceived{};
	std::chrono::steady_clock::time_point connectedSince{};

	// True when something looks off worth surfacing in the UI: outgoing not connected,
	// or outgoing connected but no inbound (likely firewall/NAT keeping the peer from
	// reaching us). The UI can use this to render a single subtle warning indicator.
	bool hasConnectivityIssue() const {
		if (outgoingConnState != PeerConnState::Connected) return true;
		return !hasIncomingConnection;
	}
};

struct PeerDisplayUpdate {
	enum class Type {
		Updated,
		Removed,
	};

	Type type{ Type::Updated };
	PeerDisplayItem item{};
};

struct PeerDisplayRegistration {
	std::size_t watcherID{};
	std::vector<PeerDisplayItem> items;
};

class PeerDisplay {
public:
	using Watcher = std::function<void(const PeerDisplayUpdate&, void*)>;

	void NotifyPeer(const std::wstring& hostName, const HostId& hostID, Peer::ConnType connType, std::chrono::steady_clock::time_point connectedSince);
	void NotifyPeerRemoved(const HostId& hostID, Peer::ConnType connType);
	void NotifyPeerBytes(const HostId& hostID, uint64_t bytesSent, uint64_t bytesReceived);
	// Outgoing-direction connection state transitions (called by Peer::ThreadProcSend).
	void NotifyPeerConnState(const std::wstring& hostName, const HostId& hostID, PeerConnState state);
	std::vector<PeerDisplayItem> Query() const;
	PeerDisplayRegistration QueryAndRegister(Watcher watcher, void* userData = nullptr);
	void Unregister(std::size_t watcherID);

private:
	struct PeerDisplayEntry {
		PeerDisplayItem item;
		std::size_t incomingConnectionCount{};
		std::size_t outgoingConnectionCount{};
	};

	static bool LessDisplayItem(const PeerDisplayItem& left, const PeerDisplayItem& right);
	static bool HostIdEquals(const PeerDisplayEntry& entry, const HostId& hostID);
	std::vector<PeerDisplayItem> SnapshotLocked() const;

	mutable std::mutex mutex_;
	std::vector<PeerDisplayEntry> entries_;
	struct WatcherRegistration {
		std::size_t watcherID{};
		Watcher watcher;
		void* userData{};
	};

	std::vector<WatcherRegistration> watchers_;
	std::size_t nextWatcherID_{ 1 };
};
