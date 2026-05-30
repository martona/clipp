#include "platform.h"

#ifdef __linux__

// Linux DNS-SD discovery.
//
// STATUS: STUB. This compiles and links so the terminal build's key/hostid verbs
// work and copy/paste fail gracefully ("could not reach any device") until the
// real backend lands.
//
// PLANNED IMPLEMENTATION (native libavahi-client + avahi_threaded_poll):
//   * Browse + resolve ONLY. Publish/register is dead on the headless build:
//     there is no daemon, and BrowseStream always calls Start(publishLocal=false).
//   * avahi_client_new(...) with no NO_FAIL flag, so a missing/!running
//     avahi-daemon fails loudly here (Start returns false, the CLI prints a clear
//     "could not reach any device" and the cause is obvious).
//   * avahi_service_browser_new(client, IF_UNSPEC, PROTO_UNSPEC, "_clipp._tcp",
//     NULL, ...). NOTE: the service type has NO ".local" suffix on Avahi (the
//     domain is the separate NULL/"local" argument), unlike the Win32 windns
//     "_clipp._tcp.local".
//   * In the browser NEW callback, fire avahi_service_resolver_new; in the
//     resolver callback you get host, port (HOST byte order, like Win32 -- not
//     network order), AvahiAddress, and AvahiStringList* txt all at once.
//   * Convert the AvahiStringList to std::map<std::string,std::string> (binary-safe
//     via avahi_string_list_get_pair) and run it through MDNSProtocol::DecodeTxt.
//     ROUTE ALL TXT THROUGH MDNSProtocol::Encode/DecodeTxt -- never hand-roll the
//     wire format, or the PacketV1 encryption + unpadded-base64 convention breaks
//     interop with Windows/macOS peers.
//   * Lift the self-filter / hostId-dedup / collision-warning bookkeeping and the
//     BrowseStream queue+condvar handoff from MDNSDiscovery_Win32.cpp essentially
//     verbatim -- that logic is platform-agnostic std::map/std::set.
//   * Locking: avahi_threaded_poll_lock/unlock around any call made off the poll
//     thread; avahi_threaded_poll_start after wiring, _stop before teardown; free
//     order resolvers -> browser -> client -> threaded_poll.

#include "MDNSDiscovery.h"
#include "Logger.h"

namespace MDNSDiscovery {

bool Start(Callback /*callback*/, bool /*publishLocal*/, bool /*includeSelf*/) {
    g_logger.log(__FUNCTION__, Logger::Level::Warning,
                 "DNS-SD discovery is not yet implemented on Linux (stub).");
    return false;
}

void Stop() {}

bool HasHostIDCollisionWarning() { return false; }

void ClearHostIDCollisionWarning() {}

bool BrowseStream(std::chrono::milliseconds /*maxWait*/, bool /*includeSelf*/,
                  const std::function<bool(const DiscoveredPeer&)>& /*onPeer*/) {
    g_logger.log(__FUNCTION__, Logger::Level::Warning,
                 "DNS-SD BrowseStream is not yet implemented on Linux (stub); "
                 "copy/paste cannot discover peers until the Avahi backend lands.");
    return false;
}

} // namespace MDNSDiscovery

#endif // __linux__
