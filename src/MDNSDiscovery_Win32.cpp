#include "platform.h"

#ifdef _WIN32

#include "MDNSDiscovery.h"
#include "MDNSProtocol.h"

#include "HostId.h"
#include "KeyManager.h"
#include "Logger.h"
#include "Settings.h"
#include "utils.h"

#include <windns.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sodium.h>

#pragma comment(lib, "dnsapi.lib")

extern Settings g_settings;

namespace MDNSDiscovery {

namespace {

constexpr wchar_t kServiceType[] = L"_clipp._tcp.local";

// ============================================================================
// Shared state
// ============================================================================
struct State {
    std::mutex mutex;
    Callback callback = nullptr;
    bool started = false;
    bool publishLocal = false;

    // Local identity (cached at Start time + on NotifyHostIDChange).
    HostId localHostId;
    std::wstring publishedInstanceNameW;   // exact name as registered (after OS conflict-resolution)
    std::string publishedInstanceNameUtf8; // lowercase, for case-insensitive compare with browse PTRs

    DNS_SERVICE_CANCEL browseCancel{};
    bool browseActive = false;

    DNS_SERVICE_CANCEL registerCancel{};
    bool registerActive = false;
    DNS_SERVICE_INSTANCE* registeredInstance = nullptr; // owned, freed at deregister

    // Instances seen in browse PTRs. Value is empty std::optional<HostId> until we successfully
    // resolve+decrypt; then it holds the hostId. We emit Removed only for instances that reached
    // the resolved state (i.e., that we previously reported Added for).
    struct InstanceState {
        bool resolved = false;
        HostId hostId;
        std::string deviceName;
    };
    std::map<std::wstring, InstanceState> liveInstances;

    // hostId-level dedup over `liveInstances`. Multiple wire instances for the
    // same hostId (e.g., after a peer restart that picks a new random instance
    // name while the old name is still in our cache awaiting TTL) collapse into
    // a single logical peer from the callback's perspective. Added fires only
    // when the set transitions empty→non-empty; Removed only on non-empty→empty.
    std::map<HostId, std::set<std::wstring>> liveByHostId;

    std::atomic<bool> hostIDCollisionWarning{ false };
};

State& GlobalState() {
    static State s;
    return s;
}

// ============================================================================
// Helpers
// ============================================================================
std::wstring Utf8ToWide(const std::string& s) { return Utf8ToWideString(s); }
std::string WideToUtf8(const std::wstring& s) { return WideToUtf8String(s); }

std::string LowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::wstring MakeInstanceName(const HostId& /*hostId*/) {
    // Non-identifying random label per publish. Random (not hostId-derived) so
    // each restart looks like a brand-new mDNS instance to neighbors — they
    // can't reuse a cached entry from a previous session and miss our re-join.
    // Identity still travels in the encrypted TXT record's hostId field; the
    // discovery layer collapses multiple instances of the same hostId into one
    // logical peer for callbacks, so the wire-level instance name can churn
    // freely without confusing PeerManager.
    unsigned char raw[4]{};
    randombytes_buf(raw, sizeof(raw));
    static const char kHex[] = "0123456789abcdef";
    std::string name = "clipp-";
    for (unsigned char b : raw) {
        name.push_back(kHex[b >> 4]);
        name.push_back(kHex[b & 0x0F]);
    }
    return Utf8ToWide(name);
}

std::wstring FullInstanceFqdn(const std::wstring& instance) {
    return instance + L"." + kServiceType;
}

void LogStatus(const char* fn, DNS_STATUS status, const char* what) {
    if (status == ERROR_SUCCESS) return;
    g_logger.log(fn, Logger::Level::Warning, "DNS-SD %s failed: status=%lu", what, static_cast<unsigned long>(status));
}

// ============================================================================
// TXT <-> DNS_SERVICE_INSTANCE conversion
// ============================================================================
DNS_SERVICE_INSTANCE* BuildLocalServiceInstance(const std::wstring& instanceName,
                                                 const std::map<std::string, std::string>& txt) {
    const std::wstring fqdn = FullInstanceFqdn(instanceName);

    // Use the local machine hostname as the SRV target — DnsServiceConstructInstance accepts
    // nullptr to mean "this host" but some implementations are happier with explicit names.
    wchar_t hostName[256] = {};
    DWORD hostLen = static_cast<DWORD>(std::size(hostName));
    if (!GetComputerNameW(hostName, &hostLen)) {
        std::wcscpy(hostName, L"localhost");
    }
    std::wstring hostFqdn = std::wstring(hostName) + L".local";

    // Build parallel arrays of wide keys/values for the TXT record.
    std::vector<std::wstring> keyStrings;
    std::vector<std::wstring> valueStrings;
    keyStrings.reserve(txt.size());
    valueStrings.reserve(txt.size());
    for (const auto& kv : txt) {
        keyStrings.push_back(Utf8ToWide(kv.first));
        valueStrings.push_back(Utf8ToWide(kv.second));
    }
    std::vector<PCWSTR> keys(txt.size());
    std::vector<PCWSTR> values(txt.size());
    for (size_t i = 0; i < txt.size(); ++i) {
        keys[i] = keyStrings[i].c_str();
        values[i] = valueStrings[i].c_str();
    }

    DNS_SERVICE_INSTANCE* instance = DnsServiceConstructInstance(
        fqdn.c_str(),
        hostFqdn.c_str(),
        nullptr, nullptr, // IPs determined by the OS at announce time
        static_cast<WORD>(g_settings.tcpPort()),
        0, 0,
        static_cast<DWORD>(txt.size()),
        keys.empty() ? nullptr : keys.data(),
        values.empty() ? nullptr : values.data());
    return instance;
}

bool TxtFromServiceInstance(const DNS_SERVICE_INSTANCE* instance,
                            std::map<std::string, std::string>& outTxt) {
    if (!instance) return false;
    for (DWORD i = 0; i < instance->dwPropertyCount; ++i) {
        if (!instance->keys || !instance->keys[i]) continue;
        const std::string key = WideToUtf8(instance->keys[i]);
        const std::string value = (instance->values && instance->values[i])
            ? WideToUtf8(instance->values[i])
            : std::string{};
        outTxt[key] = value;
    }
    return true;
}

std::string FormatIpv4(IP4_ADDRESS addr) {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr ina;
    ina.S_un.S_addr = addr;
    inet_ntop(AF_INET, &ina, buf, sizeof(buf));
    return buf;
}

std::string FormatIpv6(const IP6_ADDRESS& addr) {
    char buf[INET6_ADDRSTRLEN] = {};
    in6_addr in6;
    std::memcpy(&in6, addr.IP6Byte, sizeof(in6.s6_addr));
    inet_ntop(AF_INET6, &in6, buf, sizeof(buf));
    return buf;
}

// ============================================================================
// Resolve flow
// ============================================================================
struct ResolveContext {
    std::wstring instanceFqdn;
    DNS_SERVICE_CANCEL cancel{};
};

void HandleResolved(const DNS_SERVICE_INSTANCE* instance) {
    if (!instance || !instance->pszInstanceName) return;

    std::map<std::string, std::string> txt;
    TxtFromServiceInstance(instance, txt);

    // Diagnostic: dump TXT properties as received from the OS.
    g_logger.log(__FUNCTION__, Logger::Level::Debug,
        L"Discovery: resolved '%ls' with %lu TXT properties.",
        instance->pszInstanceName, static_cast<unsigned long>(instance->dwPropertyCount));
    for (const auto& kv : txt) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            "  TXT '%s' = '%s' (len=%zu)", kv.first.c_str(), kv.second.c_str(), kv.second.size());
    }

    MDNSProtocol::PacketV1 packet;
    if (!MDNSProtocol::DecodeTxt(txt, packet)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            L"Discovery: ignoring service '%ls' (TXT did not decrypt under our network key).",
            instance->pszInstanceName);
        return;
    }

    DiscoveredPeer peer;
    peer.deviceName = std::string(packet.deviceName);
    peer.hostId = HostId(packet.hostId);
    peer.port = instance->wPort; // Win32 dnsapi returns wPort in host byte order
    peer.osType = static_cast<OsType>(ntohs(packet.osType));
    std::memcpy(peer.caps, packet.caps, sizeof(peer.caps));

    if (instance->ip4Address) {
        peer.ip = FormatIpv4(*instance->ip4Address);
    } else if (instance->ip6Address) {
        peer.ip = FormatIpv6(*instance->ip6Address);
    }
    if (peer.ip.empty()) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            L"Discovery: resolved '%ls' but no IP available.", instance->pszInstanceName);
        return;
    }

    auto& s = GlobalState();
    Callback cb = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started) {
            // Stop() may have cleared state between when this resolve was
            // queued and now — discard the resolve to avoid touching cleared
            // maps or firing a callback after the consumer detached.
            return;
        }
        cb = s.callback;

        // Self-filter / collision detection.
        if (peer.hostId == s.localHostId) {
            const std::string instanceUtf8Lower = LowerAscii(WideToUtf8(instance->pszInstanceName));
            const bool isSelf = instanceUtf8Lower.find(s.publishedInstanceNameUtf8) == 0;
            if (!isSelf) {
                s.hostIDCollisionWarning.store(true);
                g_logger.log(__FUNCTION__, Logger::Level::Warning,
                    L"Discovery: possible host ID collision (peer '%ls' shares our host ID).",
                    instance->pszInstanceName);
            }
            return;
        }

        // Cache the resolved hostId so we can emit Removed correctly later.
        auto it = s.liveInstances.find(instance->pszInstanceName);
        if (it != s.liveInstances.end()) {
            it->second.resolved = true;
            it->second.hostId = peer.hostId;
            it->second.deviceName = peer.deviceName;
        }

        // hostId-level dedup: only fire Added on the empty→non-empty transition.
        auto& instancesForHost = s.liveByHostId[peer.hostId];
        const bool firstForHost = instancesForHost.empty();
        instancesForHost.insert(instance->pszInstanceName);
        if (!firstForHost) {
            cb = nullptr;
        }
    }

    if (cb) cb(Event::Added, peer);
}

void WINAPI OnResolveComplete(DWORD status, PVOID queryContext, PDNS_SERVICE_INSTANCE pInstance) {
    auto* ctx = static_cast<ResolveContext*>(queryContext);
    if (status == ERROR_SUCCESS && pInstance) {
        HandleResolved(pInstance);
    } else {
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            "DNS-SD resolve completed with status=%lu", static_cast<unsigned long>(status));
    }
    if (pInstance) DnsServiceFreeInstance(pInstance);
    delete ctx;
}

void StartResolve(const std::wstring& instanceFqdn) {
    auto* ctx = new ResolveContext();
    ctx->instanceFqdn = instanceFqdn;

    DNS_SERVICE_RESOLVE_REQUEST req = {};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;
    req.QueryName = ctx->instanceFqdn.data();
    req.pResolveCompletionCallback = OnResolveComplete;
    req.pQueryContext = ctx;

    const DNS_STATUS status = DnsServiceResolve(&req, &ctx->cancel);
    if (status != DNS_REQUEST_PENDING && status != ERROR_SUCCESS) {
        LogStatus(__FUNCTION__, status, "DnsServiceResolve");
        delete ctx;
    }
}

// ============================================================================
// Browse flow
// ============================================================================
void WINAPI OnBrowseCallback(DWORD status, PVOID /*queryContext*/, PDNS_RECORD pDnsRecord) {
    if (status != ERROR_SUCCESS) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            "DNS-SD browse callback status=%lu", static_cast<unsigned long>(status));
    }

    auto& s = GlobalState();
    Callback cb = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started) {
            // Final synchronous callback from DnsServiceBrowseCancel during Stop().
            // Stop() has already cleared liveInstances / callback; drop the records.
            if (pDnsRecord) DnsRecordListFree(pDnsRecord, DnsFreeRecordList);
            return;
        }
        cb = s.callback;
    }

    for (PDNS_RECORD r = pDnsRecord; r != nullptr; r = r->pNext) {
        if (r->wType != DNS_TYPE_PTR) continue;
        if (!r->Data.PTR.pNameHost) continue;

        std::wstring instanceFqdn = r->Data.PTR.pNameHost;
        const bool isGoodbye = r->dwTtl == 0;

        if (isGoodbye) {
            State::InstanceState gone;
            bool wasResolved = false;
            bool fireRemoved = false;
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                auto it = s.liveInstances.find(instanceFqdn);
                if (it != s.liveInstances.end()) {
                    gone = it->second;
                    wasResolved = gone.resolved;
                    s.liveInstances.erase(it);
                }

                if (wasResolved) {
                    // hostId-level dedup: only fire Removed on the non-empty→empty transition.
                    auto setIt = s.liveByHostId.find(gone.hostId);
                    if (setIt != s.liveByHostId.end()) {
                        setIt->second.erase(instanceFqdn);
                        if (setIt->second.empty()) {
                            s.liveByHostId.erase(setIt);
                            fireRemoved = true;
                        }
                    }
                }
            }
            if (fireRemoved && cb) {
                DiscoveredPeer peer;
                peer.deviceName = gone.deviceName;
                peer.hostId = gone.hostId;
                cb(Event::Removed, peer);
            }
            continue;
        }

        bool needResolve = false;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            // Skip our own instance.
            if (!s.publishedInstanceNameW.empty()) {
                const std::wstring expected = FullInstanceFqdn(s.publishedInstanceNameW);
                if (_wcsicmp(instanceFqdn.c_str(), expected.c_str()) == 0) {
                    continue;
                }
            }
            auto [it, inserted] = s.liveInstances.try_emplace(instanceFqdn);
            needResolve = inserted;
        }
        if (needResolve) {
            StartResolve(instanceFqdn);
        }
    }

    if (pDnsRecord) DnsRecordListFree(pDnsRecord, DnsFreeRecordList);
}

// ============================================================================
// Register flow
// ============================================================================
void WINAPI OnRegisterComplete(DWORD status, PVOID /*queryContext*/, PDNS_SERVICE_INSTANCE pInstance) {
    auto& s = GlobalState();
    if (status != ERROR_SUCCESS) {
        LogStatus(__FUNCTION__, status, "DnsServiceRegister");
    } else if (pInstance && pInstance->pszInstanceName) {
        std::wstring fullName = pInstance->pszInstanceName;
        // Strip the trailing ".<type>" if present to get the leaf instance label.
        const std::wstring suffix = std::wstring(L".") + kServiceType;
        std::wstring leaf = fullName;
        if (fullName.size() > suffix.size()
            && _wcsicmp(fullName.c_str() + fullName.size() - suffix.size(), suffix.c_str()) == 0) {
            leaf = fullName.substr(0, fullName.size() - suffix.size());
        }
        // Apply the cached published-name updates under the lock unless the
        // discovery layer has been torn down in the meantime (Stop() between
        // the register submission and this completion firing). Skipping is
        // benign — the State has already been wiped.
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.started) {
            s.publishedInstanceNameW = leaf;
            s.publishedInstanceNameUtf8 = LowerAscii(WideToUtf8(leaf));
            g_logger.log(__FUNCTION__, Logger::Level::Info,
                L"DNS-SD published as '%ls'", fullName.c_str());
        }
    }
    if (pInstance) DnsServiceFreeInstance(pInstance);
}

bool PublishLocked(State& s) {
    HostId hostId;
    if (!g_settings.getHostID(hostId)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "DNS-SD publish: host id unavailable.");
        return false;
    }
    s.localHostId = hostId;

    const std::string deviceName = MDNSProtocol::GetLocalDeviceName();
    const MDNSProtocol::PacketV1 packet = MDNSProtocol::BuildLocalPacket(deviceName, hostId, GetLocalOsType());

    std::map<std::string, std::string> txt;
    if (!MDNSProtocol::EncodeTxt(packet, txt)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            "DNS-SD publish skipped: unable to encrypt TXT (network key missing?).");
        return false;
    }

    const std::wstring instanceName = MakeInstanceName(hostId);
    DNS_SERVICE_INSTANCE* instance = BuildLocalServiceInstance(instanceName, txt);
    if (!instance) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "DNS-SD publish: DnsServiceConstructInstance failed.");
        return false;
    }

    DNS_SERVICE_REGISTER_REQUEST req = {};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;
    req.pServiceInstance = instance;
    req.pRegisterCompletionCallback = OnRegisterComplete;
    req.pQueryContext = nullptr;

    DNS_STATUS status = DnsServiceRegister(&req, &s.registerCancel);
    if (status != DNS_REQUEST_PENDING && status != ERROR_SUCCESS) {
        LogStatus(__FUNCTION__, status, "DnsServiceRegister");
        DnsServiceFreeInstance(instance);
        return false;
    }

    s.registeredInstance = instance;
    s.registerActive = true;
    return true;
}


bool StartBrowseLocked(State& s) {
    DNS_SERVICE_BROWSE_REQUEST req = {};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;
    req.QueryName = const_cast<PWSTR>(kServiceType);
    req.pBrowseCallback = OnBrowseCallback;
    req.pQueryContext = nullptr;

    DNS_STATUS status = DnsServiceBrowse(&req, &s.browseCancel);
    if (status != DNS_REQUEST_PENDING && status != ERROR_SUCCESS) {
        LogStatus(__FUNCTION__, status, "DnsServiceBrowse");
        return false;
    }
    s.browseActive = true;
    return true;
}

} // namespace

// ============================================================================
// Public API
// ============================================================================
bool Start(Callback callback, bool publishLocal) {
    auto& s = GlobalState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.started) return false;

    s.callback = callback;
    s.publishLocal = publishLocal;
    s.hostIDCollisionWarning.store(false);
    // Cache our own hostId for self-filtering, regardless of whether we publish.
    g_settings.getHostID(s.localHostId);

    if (!StartBrowseLocked(s)) {
        s.callback = nullptr;
        return false;
    }

    if (publishLocal) {
        // Publish may fail if there's no network key yet — that's OK, we'll retry on NotifyNetworkKeyChange.
        PublishLocked(s);
    }
    s.started = true;
    g_logger.log(__FUNCTION__, Logger::Level::Info, "DNS-SD discovery started.");
    return true;
}

void Stop() {
    auto& s = GlobalState();

    // Tear down all soft state under the lock, but stage the DNS_SERVICE_CANCEL
    // handles and the owned DNS_SERVICE_INSTANCE so we can invoke the actual
    // cancel APIs AFTER releasing the lock. DnsServiceBrowseCancel /
    // DnsServiceRegisterCancel synchronously deliver one final completion
    // callback on the calling thread, and those callbacks re-acquire s.mutex.
    // Holding the mutex across cancel recursively locks the non-recursive
    // std::mutex; MSVC throws system_error(EDEADLK), the exception unwinds
    // through the dnsapi C callback boundary, and terminate() fires.
    DNS_SERVICE_CANCEL browseCancel{};
    DNS_SERVICE_CANCEL registerCancel{};
    DNS_SERVICE_INSTANCE* instanceToFree = nullptr;
    bool wasBrowseActive = false;
    bool wasRegisterActive = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started) return;

        wasRegisterActive = s.registerActive;
        registerCancel = s.registerCancel;
        s.registerActive = false;
        instanceToFree = s.registeredInstance;
        s.registeredInstance = nullptr;
        s.publishedInstanceNameW.clear();
        s.publishedInstanceNameUtf8.clear();

        wasBrowseActive = s.browseActive;
        browseCancel = s.browseCancel;
        s.browseActive = false;
        s.liveInstances.clear();
        s.liveByHostId.clear();

        // Clear started + callback BEFORE the cancel calls so the synchronous
        // final callback that DnsServiceBrowseCancel/RegisterCancel delivers
        // sees the discovery layer as stopped and bails (see the !s.started
        // guards in OnBrowseCallback / HandleResolved / OnRegisterComplete).
        s.callback = nullptr;
        s.started = false;
    }

    if (wasRegisterActive) DnsServiceRegisterCancel(&registerCancel);
    if (instanceToFree)    DnsServiceFreeInstance(instanceToFree);
    if (wasBrowseActive)   DnsServiceBrowseCancel(&browseCancel);

    g_logger.log(__FUNCTION__, Logger::Level::Info, "DNS-SD discovery stopped.");
}


bool HasHostIDCollisionWarning() {
    return GlobalState().hostIDCollisionWarning.load();
}

void ClearHostIDCollisionWarning() {
    GlobalState().hostIDCollisionWarning.store(false);
}

bool BrowseOnce(std::chrono::milliseconds wait, std::vector<DiscoveredPeer>& outPeers) {
    outPeers.clear();

    // Run a private state machine so we don't disturb any active Start() session.
    struct Acc {
        std::mutex mtx;
        std::vector<DiscoveredPeer> peers;
    } acc;

    // Set a temporary callback that captures peers.
    auto& s = GlobalState();
    if (s.started) {
        // Refuse to BrowseOnce while continuous discovery is active.
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            "BrowseOnce called while continuous discovery is active; ignoring.");
        return false;
    }

    static std::mutex accSlotMutex;
    static Acc* accSlot = nullptr;
    {
        std::lock_guard<std::mutex> lock(accSlotMutex);
        accSlot = &acc;
    }

    Callback cb = [](Event ev, const DiscoveredPeer& p) {
        if (ev != Event::Added) return;
        std::lock_guard<std::mutex> lock(accSlotMutex);
        if (!accSlot) return;
        std::lock_guard<std::mutex> aLock(accSlot->mtx);
        for (const auto& existing : accSlot->peers) {
            if (existing.hostId == p.hostId) return;
        }
        accSlot->peers.push_back(p);
    };

    if (!Start(cb, /*publishLocal=*/false)) {
        std::lock_guard<std::mutex> lock(accSlotMutex);
        accSlot = nullptr;
        return false;
    }

    std::this_thread::sleep_for(wait);
    Stop();

    {
        std::lock_guard<std::mutex> lock(accSlotMutex);
        accSlot = nullptr;
    }

    std::lock_guard<std::mutex> lock(acc.mtx);
    outPeers = std::move(acc.peers);
    return true;
}

} // namespace MDNSDiscovery

#endif // _WIN32
