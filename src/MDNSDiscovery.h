#pragma once

#include "HostId.h"
#include "OsType.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// OS-backed DNS-SD peer discovery for the `_clipp._tcp` service type.
// Replaces the old custom-multicast MDNSThread.
//
// On the wire:
//  - Each device registers an instance like `clipp-<8hex>._clipp._tcp.local` whose SRV
//    record carries the TCP listener port and whose TXT record carries an encrypted
//    payload (PacketV1, see MDNSProtocol.h) with the device name, hostId, etc.
//  - Browsers receive Added/Removed callbacks as the OS observes neighbors come and go.

namespace MDNSDiscovery {

struct DiscoveredPeer {
    std::string deviceName;     // human-readable; from the encrypted TXT payload
    std::string ip;             // resolved IPv4 / IPv6 in textual form
    HostId      hostId;
    uint16_t    port = 0;       // from the SRV record
    OsType      osType = OsType::Unknown;
    uint8_t     caps[8] = {};   // reserved capability bits (currently always 0)
};

enum class Event {
    Added,      // peer just became reachable; PeerManager should attempt to connect
    Removed,    // OS reports peer left (goodbye / TTL); abandon outgoing connection attempts
};

using Callback = void(*)(Event, const DiscoveredPeer&);

// Starts continuous browse + (optionally) publish for the local device.
// `publishLocal=false` is for browse-only contexts (iOS share extension).
// `includeSelf=true` surfaces peers that share our own hostId (this device's own
// running GUI) instead of filtering them out; the continuous runtime leaves it false.
bool Start(Callback callback, bool publishLocal = true, bool includeSelf = false);

// Stops browse + publish. Sends DNS-SD goodbye for the published instance.
void Stop();

// Host id collision warning is raised when we see another peer advertising our hostId.
bool HasHostIDCollisionWarning();
void ClearHostIDCollisionWarning();

// Streaming one-shot browse for the one-shot verbs (CLI `copy`/`paste`, iOS share
// extension). Invokes `onPeer` for each peer as it resolves, on the *calling* thread
// (so onPeer may do blocking network I/O). `onPeer` returns true to keep browsing or
// false to stop immediately — callers use a single discovered peer as a gateway to
// the synced mesh, so they stop at the first peer the operation succeeds with. Returns
// true if `onPeer` stopped it (success), false if `maxWait` elapsed with no stop.
// `includeSelf=true` surfaces this device's own GUI (same hostId); the share extension
// leaves it false (it can't assume its own app is running to relay).
bool BrowseStream(std::chrono::milliseconds maxWait, bool includeSelf,
                  const std::function<bool(const DiscoveredPeer&)>& onPeer);

} // namespace MDNSDiscovery

// Back-compat aliases for callers that haven't been ported yet (NetworkPage /
// SettingsPage / iOS bridge).
inline bool MDNSHasHostIDCollisionWarning() { return MDNSDiscovery::HasHostIDCollisionWarning(); }
inline void MDNSClearHostIDCollisionWarning() { MDNSDiscovery::ClearHostIDCollisionWarning(); }
