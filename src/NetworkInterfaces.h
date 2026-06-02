#pragma once

#include <cstdint>

// Cross-platform snapshot of the local host's network reachability, used to decide
// when to re-announce our DNS-SD service after an interface change (cable swap, Wi-Fi
// toggle, VPN/overlay renumber, ...).
//
// Deliberately minimal filtering: UP + running + non-loopback, nothing else. We do
// NOT try to classify "real" vs "virtual"/"tunnel" interfaces -- a Clipp mesh can run
// *over* Tailscale/WireGuard, so a tunnel address change is exactly the kind of event
// we must react to. The action this gates (a republish) is cheap, so we err toward
// over-detecting; the only goal here is to collapse "did my advertised address set
// change" into one stable number.

namespace clipp {

// xxhash (XXH3-64) of the sorted set of unicast IPv4/IPv6 addresses on every UP,
// running, non-loopback interface, each tagged with its interface name so a
// same-address move between interfaces still registers. Returns 0 on enumeration
// failure (treated by callers as "unknown / no change" -- a transient failure must
// not be mistaken for "all interfaces vanished").
uint64_t HashLocalInterfaceAddresses();

}  // namespace clipp
