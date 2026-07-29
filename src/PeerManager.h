#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "DefensiveLogThrottle.h"
#include "Peer.h"

class PeerManager {
public:
	enum class IncomingPeerAdmission {
		Accepted,
		RejectedMissingAddress,
		RejectedGlobalAuthLimit,
		RejectedPerIpAuthLimit,
	};
	enum class IncomingPeerEstablishment {
		Accepted,
		AcceptedFirst,
		RejectedPeerLimit,
	};
	enum class OutgoingPeerAdmission {
		Accepted,
		AlreadyPresent,
		RejectedPeerLimit,
	};

	PeerManager();
	~PeerManager();
	OutgoingPeerAdmission AddPeer(const wchar_t* hostName, const HostId& hostID, const wchar_t* ip, unsigned short port);
	// Takes ownership of socket in every case. Rejected sockets are closed without
	// constructing a Peer or starting its worker thread.
	IncomingPeerAdmission AddIncomingPeer(SOCKET socket, Peer::ClipboardReceivedCallback clipboardReceivedCallback);
	void RemovePeer(const HostId& hostID);
	// Tears down only the outgoing connection for this hostId, leaving any inbound connection
	// (e.g., from an iOS share extension running alongside the same hostId) intact.
	void RemoveOutgoingPeer(const HostId& hostID);
	void CullPeers();
	void ClearPeers();
	// Returns the number of live peer connections the payload was queued to
	// (0 = nobody to hand it to — callers use this to keep send feedback honest).
	size_t BroadcastClipboard(std::shared_ptr<const ClipboardPayload> payload);
	// Send a pre-encoded register frame to every connected peer advertising
	// CAP0_SERVES_REGISTERS — rebroadcasts a re-stamped relay write to the mesh.
	void BroadcastRegisterFrame(const std::array<char, 4>& tag, const std::vector<unsigned char>& body);
	// Send a pre-encoded frame to EVERY connected peer, no capability gating —
	// for frames old builds tolerate (unknown tags are log-and-ignore), e.g. the
	// CDEL activity-delete broadcast.
	void BroadcastFrame(const std::array<char, 4>& tag, const std::vector<unsigned char>& body);

	// Admits at most 127 simultaneously authenticated incoming peers. AcceptedFirst
	// marks the 0→1 transition used by the activity-stream sync trigger. Every
	// accepted result must be paired with one OnIncomingPeerLeft call.
	IncomingPeerEstablishment TryEstablishIncomingPeer();
	void OnIncomingPeerLeft();
private:
	mutable std::mutex peersMutex_;
	std::vector<std::unique_ptr<Peer>> peers_;
	std::mutex incomingCountMutex_;
	std::size_t establishedIncomingCount_{ 0 };
	DefensiveLogThrottle globalPendingAuthLog_;
	DefensiveLogThrottle perIpPendingAuthLog_;
	DefensiveLogThrottle establishedIncomingLog_;
	DefensiveLogThrottle outgoingPeerLog_;
};
