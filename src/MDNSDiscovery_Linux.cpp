#include "platform.h"

#ifdef __linux__

// Linux DNS-SD discovery via native libavahi-client.
//
// SCOPE: BrowseStream (the one-shot browse+resolve the CLI copy/paste verbs use) is
// implemented. Start/Stop (the continuous browse+publish the GUI daemon uses) are
// intentional stubs: the headless Linux build compiles out NetworkRuntime, the only
// caller, so nothing here ever publishes a service or runs a continuous browse.
//
// All TXT encode/decode goes through the shared MDNSProtocol::Encode/DecodeTxt so the
// PacketV1 encryption + unpadded-base64 convention stays identical across platforms
// -- this file only marshals an AvahiStringList <-> std::map<string,string>.

#include "MDNSDiscovery.h"
#include "MDNSProtocol.h"
#include "HostId.h"
#include "Logger.h"
#include "Settings.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-common/thread-watch.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

extern Settings g_settings;

namespace MDNSDiscovery {

namespace {

constexpr char kServiceType[] = "_clipp._tcp";  // no ".local": Avahi takes the domain separately

// One BrowseStream call's shared state. Lives on BrowseStream's stack; the Avahi
// threaded-poll thread (which runs the browse/resolve callbacks) pushes resolved
// peers into `pending`, and the BrowseStream thread drains them and runs onPeer.
struct BrowseSession {
    AvahiThreadedPoll* poll = nullptr;
    AvahiClient* client = nullptr;
    AvahiServiceBrowser* browser = nullptr;

    bool includeSelf = false;
    HostId localHostId;

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<DiscoveredPeer> pending;
    std::set<HostId> deliveredHostIds;  // hostId-level dedup (one logical peer, one onPeer)
    bool failed = false;                // client/browser failure: wake the waiter so it bails
};

// Resolve completion (on the poll thread). On success: marshal TXT -> map, decrypt via
// the shared protocol, and queue the peer. Frees its own resolver before returning.
void ResolveCallback(AvahiServiceResolver* resolver, AvahiIfIndex, AvahiProtocol,
                     AvahiResolverEvent event, const char* name, const char* /*type*/,
                     const char* /*domain*/, const char* /*hostName*/,
                     const AvahiAddress* address, uint16_t port,
                     AvahiStringList* txt, AvahiLookupResultFlags, void* userdata) {
    auto* session = static_cast<BrowseSession*>(userdata);

    // Only INET addresses are usable: OneShotPeer::Connect is IPv4-only (inet_pton
    // AF_INET). We also requested AVAHI_PROTO_INET, so this is belt-and-suspenders.
    if (event == AVAHI_RESOLVER_FOUND && address != nullptr && address->proto == AVAHI_PROTO_INET) {
        std::map<std::string, std::string> txtMap;
        for (AvahiStringList* entry = txt; entry != nullptr; entry = avahi_string_list_get_next(entry)) {
            char* key = nullptr;
            char* value = nullptr;
            size_t valueSize = 0;
            if (avahi_string_list_get_pair(entry, &key, &value, &valueSize) == 0) {
                txtMap[key] = std::string(value != nullptr ? value : "", value != nullptr ? valueSize : 0);
                avahi_free(key);
                avahi_free(value);
            }
        }

        MDNSProtocol::PacketV1 packet;
        if (MDNSProtocol::DecodeTxt(txtMap, packet)) {
            char addressText[AVAHI_ADDRESS_STR_MAX] = {};
            avahi_address_snprint(addressText, sizeof(addressText), address);

            DiscoveredPeer peer;
            peer.deviceName = std::string(packet.deviceName);
            peer.hostId = HostId(packet.hostId);
            peer.ip = addressText;
            peer.port = port;  // Avahi reports host byte order; DiscoveredPeer wants host order
            peer.osType = static_cast<OsType>(ntohs(packet.osType));
            std::memcpy(peer.caps, packet.caps, sizeof(peer.caps));

            if (!peer.ip.empty()) {
                std::lock_guard<std::mutex> lock(session->mutex);
                const bool isSelf = (peer.hostId == session->localHostId);
                if ((session->includeSelf || !isSelf) &&
                    session->deliveredHostIds.find(peer.hostId) == session->deliveredHostIds.end()) {
                    session->deliveredHostIds.insert(peer.hostId);
                    session->pending.push_back(std::move(peer));
                    session->cv.notify_all();
                }
            }
        } else {
            g_logger.log(__FUNCTION__, Logger::Level::Debug,
                "Discovery: ignoring '%s' (TXT did not decrypt under our network key).",
                name != nullptr ? name : "?");
        }
    }

    avahi_service_resolver_free(resolver);
}

// Browse event (on the poll thread). NEW -> kick off a resolve; FAILURE -> wake the
// waiter. The headless one-shot ignores REMOVE / CACHE_EXHAUSTED / ALL_FOR_NOW.
void BrowseCallback(AvahiServiceBrowser* /*browser*/, AvahiIfIndex interface, AvahiProtocol protocol,
                    AvahiBrowserEvent event, const char* name, const char* type,
                    const char* domain, AvahiLookupResultFlags, void* userdata) {
    auto* session = static_cast<BrowseSession*>(userdata);
    switch (event) {
    case AVAHI_BROWSER_NEW:
        // Resolve IPv4 (AVAHI_PROTO_INET) to match the IPv4-only connect path. The
        // resolver frees itself in ResolveCallback. A null return just means this
        // instance won't resolve; the browse continues.
        avahi_service_resolver_new(session->client, interface, protocol, name, type, domain,
                                   AVAHI_PROTO_INET, static_cast<AvahiLookupFlags>(0),
                                   ResolveCallback, session);
        break;
    case AVAHI_BROWSER_FAILURE: {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, "Discovery: browse failed: %s",
                     avahi_strerror(avahi_client_errno(session->client)));
        std::lock_guard<std::mutex> lock(session->mutex);
        session->failed = true;
        session->cv.notify_all();
        break;
    }
    default:
        break;
    }
}

// Client state (on the poll thread). Only a hard failure matters for the one-shot.
void ClientCallback(AvahiClient* /*client*/, AvahiClientState state, void* userdata) {
    auto* session = static_cast<BrowseSession*>(userdata);
    if (state == AVAHI_CLIENT_FAILURE) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->failed = true;
        session->cv.notify_all();
    }
}

}  // namespace

// ============================================================================
// Public API
// ============================================================================

// Continuous discovery is daemon-only; the headless build never calls these (the
// sole caller, NetworkRuntime, is compiled out). Kept as defined stubs so the
// MDNSDiscovery.h contract is satisfied.
bool Start(Callback /*callback*/, bool /*publishLocal*/, bool /*includeSelf*/) {
    g_logger.log(__FUNCTION__, Logger::Level::Warning,
                 "Continuous DNS-SD discovery is not supported on the headless Linux build.");
    return false;
}

void Stop() {}

bool HasHostIDCollisionWarning() { return false; }

void ClearHostIDCollisionWarning() {}

bool BrowseStream(std::chrono::milliseconds maxWait, bool includeSelf,
                  const std::function<bool(const DiscoveredPeer&)>& onPeer) {
    BrowseSession session;
    session.includeSelf = includeSelf;
    g_settings.getHostID(session.localHostId);  // for self-filter; all-zero if none, harmless

    session.poll = avahi_threaded_poll_new();
    if (session.poll == nullptr) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Discovery: avahi_threaded_poll_new failed.");
        return false;
    }

    int error = 0;
    session.client = avahi_client_new(avahi_threaded_poll_get(session.poll),
                                      static_cast<AvahiClientFlags>(0),
                                      ClientCallback, &session, &error);
    if (session.client == nullptr) {
        // The common cause is no running avahi-daemon -- surface it loudly (by design).
        g_logger.log(__FUNCTION__, Logger::Level::Error,
                     "Discovery: avahi_client_new failed: %s. Is avahi-daemon running?",
                     avahi_strerror(error));
        avahi_threaded_poll_free(session.poll);
        return false;
    }

    session.browser = avahi_service_browser_new(session.client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                                                kServiceType, nullptr, static_cast<AvahiLookupFlags>(0),
                                                BrowseCallback, &session);
    if (session.browser == nullptr) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Discovery: avahi_service_browser_new failed: %s",
                     avahi_strerror(avahi_client_errno(session.client)));
        avahi_client_free(session.client);
        avahi_threaded_poll_free(session.poll);
        return false;
    }

    if (avahi_threaded_poll_start(session.poll) < 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Discovery: avahi_threaded_poll_start failed.");
        avahi_service_browser_free(session.browser);
        avahi_client_free(session.client);
        avahi_threaded_poll_free(session.poll);
        return false;
    }

    // Drain resolved peers and hand each to onPeer on THIS thread (so onPeer may
    // block on network I/O), stopping at the first that returns false or when maxWait
    // elapses with nothing more to try. Mirrors the Win32 BrowseStream loop.
    const auto deadline = std::chrono::steady_clock::now() + maxWait;
    bool stopped = false;
    {
        std::unique_lock<std::mutex> lock(session.mutex);
        while (!stopped) {
            if (session.pending.empty()) {
                if (session.failed) {
                    break;  // client/browser died and nothing queued: give up
                }
                if (session.cv.wait_until(lock, deadline) == std::cv_status::timeout && session.pending.empty()) {
                    break;  // maxWait elapsed with nothing (more) to try
                }
            }
            std::vector<DiscoveredPeer> batch;
            batch.swap(session.pending);
            lock.unlock();
            for (const DiscoveredPeer& peer : batch) {
                if (!onPeer(peer)) {
                    stopped = true;
                    break;
                }
            }
            lock.lock();
        }
    }

    // Stop the poll thread BEFORE freeing, so no callback fires mid-teardown. Freeing
    // the client also frees any resolvers still outstanding from BrowseCallback.
    avahi_threaded_poll_stop(session.poll);
    avahi_service_browser_free(session.browser);
    avahi_client_free(session.client);
    avahi_threaded_poll_free(session.poll);
    return stopped;
}

}  // namespace MDNSDiscovery

#endif // __linux__
