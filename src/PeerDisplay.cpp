#include "PeerDisplay.h"

#include <algorithm>
#include <utility>

bool PeerDisplay::LessDisplayItem(const PeerDisplayItem& left, const PeerDisplayItem& right) {
	return left.hostID < right.hostID;
}

bool PeerDisplay::HostIdEquals(const PeerDisplayEntry& entry, const HostId& hostID) {
	return entry.item.hostID == hostID;
}

std::vector<PeerDisplayItem> PeerDisplay::SnapshotLocked() const {
	std::vector<PeerDisplayItem> snapshot;
	snapshot.reserve(entries_.size());
	for (const auto& entry : entries_) {
		snapshot.push_back(entry.item);
	}
	return snapshot;
}

std::size_t PeerDisplay::ConnectedCount() const {
	std::lock_guard<std::mutex> lock(mutex_);
	std::size_t count = 0;
	for (const auto& entry : entries_) {
		const PeerDisplayItem& item = entry.item;
		// "Connected" = we can move clipboard data with this peer right now: either a
		// completed outgoing handshake or a live inbound socket. A peer merely in
		// Connecting/Backoff/Failed with no inbound link does not count.
		if (item.outgoingConnState == PeerConnState::Connected || item.hasIncomingConnection) {
			++count;
		}
	}
	return count;
}

void PeerDisplay::NotifyPeer(const std::wstring& hostName, const HostId& hostID, OsType osType, Peer::ConnType connType, std::chrono::steady_clock::time_point connectedSince) {
	PeerDisplayUpdate update;
	std::vector<WatcherRegistration> watchers;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto found = std::find_if(entries_.begin(), entries_.end(), [&hostID](const PeerDisplayEntry& entry) {
			return HostIdEquals(entry, hostID);
		});

		if (found == entries_.end()) {
			PeerDisplayEntry entry;
			entry.item.hostName = hostName;
			entry.item.hostID = hostID;
			found = entries_.insert(std::upper_bound(entries_.begin(), entries_.end(), entry.item,
				[](const PeerDisplayItem& item, const PeerDisplayEntry& entry) { return LessDisplayItem(item, entry.item); }), entry);
		} else if (!hostName.empty()) {
			found->item.hostName = hostName;
		}

		// Once learned, the peer's OS is permanent for this entry. A divergent later
		// report (inbound vs outbound) is treated as a bug and ignored, not reconciled.
		if (found->item.osType == OsType::Unknown && osType != OsType::Unknown) {
			found->item.osType = osType;
		}

		if (connType == Peer::ConnType::Incoming) {
			++found->item.incomingConnectionCount;
		} else {
			++found->item.outgoingConnectionCount;
		}
		found->item.hasIncomingConnection = found->item.incomingConnectionCount > 0;
		found->item.hasOutgoingConnection = found->item.outgoingConnectionCount > 0;
		if (found->item.connectedSince == std::chrono::steady_clock::time_point{} || connectedSince < found->item.connectedSince) {
			found->item.connectedSince = connectedSince;
		}

		update.type = PeerDisplayUpdate::Type::Updated;
		update.item = found->item;

		std::sort(entries_.begin(), entries_.end(), [](const PeerDisplayEntry& left, const PeerDisplayEntry& right) {
			return LessDisplayItem(left.item, right.item);
		});
		watchers = watchers_;
	}

	for (const auto& watcher : watchers) {
		if (watcher.watcher) {
			watcher.watcher(update, watcher.userData);
		}
	}
}

void PeerDisplay::NotifyPeerRemoved(const HostId& hostID, Peer::ConnType connType) {
	PeerDisplayUpdate update;
	std::vector<WatcherRegistration> watchers;
	bool shouldNotify = false;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto found = std::find_if(entries_.begin(), entries_.end(), [&hostID](const PeerDisplayEntry& entry) {
			return HostIdEquals(entry, hostID);
		});
		if (found == entries_.end()) {
			return;
		}

		if (connType == Peer::ConnType::Incoming) {
			if (found->item.incomingConnectionCount > 0) --found->item.incomingConnectionCount;
		} else {
			if (found->item.outgoingConnectionCount > 0) --found->item.outgoingConnectionCount;
		}
		found->item.hasIncomingConnection = found->item.incomingConnectionCount > 0;
		found->item.hasOutgoingConnection = found->item.outgoingConnectionCount > 0;

		if (found->item.incomingConnectionCount > 0 || found->item.outgoingConnectionCount > 0) {
			update.type = PeerDisplayUpdate::Type::Updated;
			update.item = found->item;
		} else {
			update.type = PeerDisplayUpdate::Type::Removed;
			update.item = found->item;
			update.item.hasIncomingConnection = false;
			update.item.hasOutgoingConnection = false;
			update.item.connectedSince = std::chrono::steady_clock::time_point{};
			entries_.erase(found);
		}

		shouldNotify = true;
		watchers = watchers_;
	}

	if (!shouldNotify) return;
	for (const auto& watcher : watchers) {
		if (watcher.watcher) {
			watcher.watcher(update, watcher.userData);
		}
	}
}

void PeerDisplay::NotifyPeerConnState(const std::wstring& hostName, const HostId& hostID, PeerConnState state) {
	PeerDisplayUpdate update;
	std::vector<WatcherRegistration> watchers;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto found = std::find_if(entries_.begin(), entries_.end(), [&hostID](const PeerDisplayEntry& entry) {
			return HostIdEquals(entry, hostID);
		});

		if (found == entries_.end()) {
			// Create a placeholder entry so the UI can render "Connecting…" before the first
			// successful handshake. NotifyPeer will fill in any missing data when it fires.
			PeerDisplayEntry entry;
			entry.item.hostName = hostName;
			entry.item.hostID = hostID;
			entry.item.outgoingConnState = state;
			found = entries_.insert(std::upper_bound(entries_.begin(), entries_.end(), entry.item,
				[](const PeerDisplayItem& item, const PeerDisplayEntry& entry) { return LessDisplayItem(item, entry.item); }), entry);
		} else {
			if (!hostName.empty() && found->item.hostName.empty()) {
				found->item.hostName = hostName;
			}
			found->item.outgoingConnState = state;
		}

		update.type = PeerDisplayUpdate::Type::Updated;
		update.item = found->item;
		watchers = watchers_;
	}

	for (const auto& watcher : watchers) {
		if (watcher.watcher) {
			watcher.watcher(update, watcher.userData);
		}
	}
}

void PeerDisplay::NotifyPeerBytes(const HostId& hostID, uint64_t bytesSent, uint64_t bytesReceived) {
	PeerDisplayUpdate update;
	std::vector<WatcherRegistration> watchers;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto found = std::find_if(entries_.begin(), entries_.end(), [&hostID](const PeerDisplayEntry& entry) {
			return HostIdEquals(entry, hostID);
		});
		if (found == entries_.end()) {
			return;
		}

		found->item.bytesSent += bytesSent;
		found->item.bytesReceived += bytesReceived;

		update.type = PeerDisplayUpdate::Type::Updated;
		update.item = found->item;
		watchers = watchers_;
	}

	for (const auto& watcher : watchers) {
		if (watcher.watcher) {
			watcher.watcher(update, watcher.userData);
		}
	}
}

std::vector<PeerDisplayItem> PeerDisplay::Query() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return SnapshotLocked();
}

PeerDisplayRegistration PeerDisplay::QueryAndRegister(Watcher watcher, void* userData) {
	std::lock_guard<std::mutex> lock(mutex_);
	PeerDisplayRegistration registration;
	registration.watcherID = nextWatcherID_++;
	registration.items = SnapshotLocked();
	watchers_.push_back({ registration.watcherID, std::move(watcher), userData });
	return registration;
}

void PeerDisplay::Unregister(std::size_t watcherID) {
	std::lock_guard<std::mutex> lock(mutex_);
	watchers_.erase(std::remove_if(watchers_.begin(), watchers_.end(), [watcherID](const auto& watcher) {
		return watcher.watcherID == watcherID;
	}), watchers_.end());
}
