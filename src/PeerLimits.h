#pragma once

#include <cstddef>

namespace PeerLimits {

inline constexpr std::size_t MaxPendingIncomingAuthentications = 32;
inline constexpr std::size_t MaxPendingIncomingAuthenticationsPerIp = 8;
inline constexpr std::size_t MaxAuthenticatedIncomingPeers = 127;
inline constexpr std::size_t MaxOutgoingPeers = 127;

enum class PendingAuthenticationAdmission {
	Accepted,
	RejectedGlobalLimit,
	RejectedPerIpLimit,
};

constexpr PendingAuthenticationAdmission EvaluatePendingAuthentication(
	std::size_t pendingGlobal,
	std::size_t pendingForIp) {
	if (pendingGlobal >= MaxPendingIncomingAuthentications) {
		return PendingAuthenticationAdmission::RejectedGlobalLimit;
	}
	if (pendingForIp >= MaxPendingIncomingAuthenticationsPerIp) {
		return PendingAuthenticationAdmission::RejectedPerIpLimit;
	}
	return PendingAuthenticationAdmission::Accepted;
}

constexpr bool CanEstablishIncoming(std::size_t establishedIncoming) {
	return establishedIncoming < MaxAuthenticatedIncomingPeers;
}

constexpr bool CanAddOutgoing(std::size_t outgoingPeers) {
	return outgoingPeers < MaxOutgoingPeers;
}

}  // namespace PeerLimits
