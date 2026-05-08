#include "PeerDisplay.h"

#include <algorithm>
#include <utility>

bool PeerDisplay::LessDisplayItem(const PeerDisplayItem& left, const PeerDisplayItem& right) {
	if (left.hostID != right.hostID) {
		return std::lexicographical_compare(left.hostID.begin(), left.hostID.end(), right.hostID.begin(), right.hostID.end());
	}
	return left.hostName < right.hostName;
}

bool PeerDisplay::HostIdEquals(const PeerDisplayEntry& entry, const std::array<unsigned char, 32>& hostID) {
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

void PeerDisplay::NotifyPeer(const std::wstring& hostName, const std::array<unsigned char, 32>& hostID, Peer::ConnType connType, std::chrono::steady_clock::time_point connectedSince) {
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

		if (connType == Peer::ConnType::Incoming) {
			++found->incomingConnectionCount;
		} else {
			++found->outgoingConnectionCount;
		}
		found->item.hasIncomingConnection = found->incomingConnectionCount > 0;
		found->item.hasOutgoingConnection = found->outgoingConnectionCount > 0;
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

void PeerDisplay::NotifyPeerRemoved(const std::wstring& hostName, const std::array<unsigned char, 32>& hostID, Peer::ConnType connType) {
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
			if (found->incomingConnectionCount > 0) --found->incomingConnectionCount;
		} else {
			if (found->outgoingConnectionCount > 0) --found->outgoingConnectionCount;
		}
		found->item.hasIncomingConnection = found->incomingConnectionCount > 0;
		found->item.hasOutgoingConnection = found->outgoingConnectionCount > 0;

		if (!hostName.empty()) {
			found->item.hostName = hostName;
		}

		if (found->incomingConnectionCount > 0 || found->outgoingConnectionCount > 0) {
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

void PeerDisplay::NotifyPeerBytes(const std::wstring& hostName, const std::array<unsigned char, 32>& hostID, uint64_t bytesSent, uint64_t bytesReceived) {
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

		if (!hostName.empty()) {
			found->item.hostName = hostName;
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
