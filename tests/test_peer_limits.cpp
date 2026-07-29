#include <doctest/doctest.h>

#include "PeerLimits.h"

TEST_CASE("pending authentication limits accept the last available slots") {
	CHECK(PeerLimits::MaxPendingIncomingAuthentications == 32);
	CHECK(PeerLimits::MaxPendingIncomingAuthenticationsPerIp == 8);

	CHECK(
		PeerLimits::EvaluatePendingAuthentication(31, 7)
		== PeerLimits::PendingAuthenticationAdmission::Accepted);
}

TEST_CASE("pending authentication limits reject at their boundaries") {
	CHECK(
		PeerLimits::EvaluatePendingAuthentication(32, 0)
		== PeerLimits::PendingAuthenticationAdmission::RejectedGlobalLimit);
	CHECK(
		PeerLimits::EvaluatePendingAuthentication(0, 8)
		== PeerLimits::PendingAuthenticationAdmission::RejectedPerIpLimit);

	// Preserve PeerManager's global-before-per-IP rejection precedence.
	CHECK(
		PeerLimits::EvaluatePendingAuthentication(32, 8)
		== PeerLimits::PendingAuthenticationAdmission::RejectedGlobalLimit);
}

TEST_CASE("authenticated incoming and outgoing peer limits reject peer 128") {
	CHECK(PeerLimits::MaxAuthenticatedIncomingPeers == 127);
	CHECK(PeerLimits::MaxOutgoingPeers == 127);

	CHECK(PeerLimits::CanEstablishIncoming(126));
	CHECK_FALSE(PeerLimits::CanEstablishIncoming(127));

	CHECK(PeerLimits::CanAddOutgoing(126));
	CHECK_FALSE(PeerLimits::CanAddOutgoing(127));
}
