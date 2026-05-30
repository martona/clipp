#include "platform.h"

#ifdef __APPLE__

#include "MDNSDiscovery.h"
#include "MDNSProtocol.h"

#include "HostId.h"
#include "KeyManager.h"
#include "Logger.h"
#include "Settings.h"
#include "utils.h"

#import <Foundation/Foundation.h>
// NSNetService and its TXT-record helpers live here; the Foundation umbrella does not
// always re-export them on newer SDKs.
#import <Foundation/NSNetServices.h>

#if !__has_feature(objc_arc)
#error "MDNSDiscovery_Apple.mm requires ARC. Enable CLANG_ENABLE_OBJC_ARC on the target."
#endif

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

extern Settings g_settings;

// Defined here so both the C++ namespace and the ObjC coordinator can refer to it.
struct ResolvedInstance {
    bool resolved = false;
    HostId hostId;
    std::string deviceName;
};

namespace MDNSDiscovery {

namespace {

constexpr const char* kServiceType = "_clipp._tcp.";
constexpr const char* kServiceDomain = "local.";

// ============================================================================
// C++ shared state (mirrors the Win32 impl)
// ============================================================================
struct State {
    std::mutex mutex;
    Callback callback = nullptr;
    bool started = false;
    bool publishLocal = false;
    bool includeSelf = false;   // BrowseOnce(includeSelf): surface same-hostId peers (the local GUI)
    HostId localHostId;
    std::string publishedInstanceName;
    std::atomic<bool> hostIDCollisionWarning{ false };

    // Tracks instance-name -> hostId mapping so didRemoveService can emit a usable hostId.
    std::mutex liveInstancesMutex;
    std::map<std::string, ResolvedInstance> liveInstances;

    // hostId-level dedup over `liveInstances`. Multiple wire instances for the
    // same hostId (e.g., after a peer restart that picks a new random instance
    // name while the old name is still in our cache awaiting TTL) collapse into
    // a single logical peer from the callback's perspective. Added fires only
    // when the set transitions empty→non-empty; Removed only on non-empty→empty.
    // Guarded by liveInstancesMutex (same scope as liveInstances).
    std::map<HostId, std::set<std::string>> liveByHostId;
};

State& GlobalState() {
    static State s;
    return s;
}

std::string LowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string MakeInstanceName(const HostId& /*hostId*/) {
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
    return name;
}

// RFC 6763 TXT record format: a sequence of <1-byte length><up to 255 bytes of "key=value">.
// NSNetService's +dataFromTXTRecord: / +dictionaryFromTXTRecord: helpers are deprecated and
// not reliably visible on newer SDKs, so we build/parse the wire bytes directly.
NSData* BuildTxtRecordData(const std::map<std::string, std::string>& txt) {
    NSMutableData* data = [NSMutableData data];
    for (const auto& kv : txt) {
        const std::string entry = kv.first + "=" + kv.second;
        if (entry.size() > 255) continue;
        const uint8_t len = static_cast<uint8_t>(entry.size());
        [data appendBytes:&len length:1];
        [data appendBytes:entry.data() length:entry.size()];
    }
    return data;
}

std::map<std::string, std::string> ParseTxtRecordData(NSData* data) {
    std::map<std::string, std::string> result;
    if (!data || data.length == 0) return result;
    const uint8_t* bytes = static_cast<const uint8_t*>(data.bytes);
    NSUInteger offset = 0;
    while (offset < data.length) {
        const uint8_t len = bytes[offset++];
        if (offset + len > data.length) break;
        std::string entry(reinterpret_cast<const char*>(bytes + offset), len);
        offset += len;
        const auto eqPos = entry.find('=');
        if (eqPos == std::string::npos) {
            result[entry] = "";
        } else {
            result[entry.substr(0, eqPos)] = entry.substr(eqPos + 1);
        }
    }
    return result;
}

} // namespace
} // namespace MDNSDiscovery

// ============================================================================
// Objective-C delegate
// ============================================================================
@interface ClippMDNSAppleCoordinator : NSObject <NSNetServiceBrowserDelegate, NSNetServiceDelegate>
@property (nonatomic, strong) NSNetServiceBrowser* browser;
@property (nonatomic, strong) NSNetService* publishedService;
@property (nonatomic, copy) NSString* publishedNameLower;
@property (nonatomic, strong) NSMutableSet<NSNetService*>* resolving;
@property (nonatomic, strong) NSThread* thread;
@property (nonatomic, assign) BOOL stopRequested;
@property (nonatomic, assign) std::map<std::string, ResolvedInstance>* liveInstances;
@property (nonatomic, assign) std::map<HostId, std::set<std::string>>* liveByHostId;
@property (nonatomic, assign) std::mutex* liveInstancesMutex;

// Optional capture target: when set, Added events also push into this vector (used by BrowseOnce).
@property (nonatomic, assign) std::vector<MDNSDiscovery::DiscoveredPeer>* captureVector;
@property (nonatomic, assign) std::mutex* captureMutex;

- (void)startContinuousPublish:(BOOL)publishLocal;
- (void)stopContinuous;
@end

@implementation ClippMDNSAppleCoordinator

- (instancetype)init {
    self = [super init];
    if (self) {
        _resolving = [NSMutableSet new];
    }
    return self;
}

static std::string LowerCaseUtf8FromNSString(NSString* s) {
    NSString* lower = [s lowercaseString];
    return lower.UTF8String ? lower.UTF8String : "";
}

#pragma mark - Browse / publish lifecycle

- (void)setupBrowserAndPublishOnCurrentRunLoop:(BOOL)publishLocal {
    NSRunLoop* runLoop = [NSRunLoop currentRunLoop];

    _browser = [[NSNetServiceBrowser alloc] init];
    _browser.delegate = self;
    [_browser scheduleInRunLoop:runLoop forMode:NSRunLoopCommonModes];
    [_browser searchForServicesOfType:@(MDNSDiscovery::kServiceType)
                              inDomain:@(MDNSDiscovery::kServiceDomain)];

    if (publishLocal) {
        [self publishLocalOnRunLoop:runLoop];
    }
}

- (void)publishLocalOnRunLoop:(NSRunLoop*)runLoop {
    auto& s = MDNSDiscovery::GlobalState();
    HostId hostId;
    if (!g_settings.getHostID(hostId)) {
        g_logger.log("MDNSDiscovery::publish", Logger::Level::Error, "DNS-SD: no host id, cannot publish.");
        return;
    }
    s.localHostId = hostId;

    const std::string deviceName = MDNSProtocol::GetLocalDeviceName();
    const MDNSProtocol::PacketV1 packet =
        MDNSProtocol::BuildLocalPacket(deviceName, hostId, GetLocalOsType());

    std::map<std::string, std::string> txt;
    if (!MDNSProtocol::EncodeTxt(packet, txt)) {
        g_logger.log("MDNSDiscovery::publish", Logger::Level::Debug,
            "DNS-SD: skipping publish (no network key yet).");
        return;
    }

    const std::string instanceName = MDNSDiscovery::MakeInstanceName(hostId);
    s.publishedInstanceName = MDNSDiscovery::LowerAscii(instanceName);
    self.publishedNameLower = @(s.publishedInstanceName.c_str());

    NSNetService* service =
        [[NSNetService alloc] initWithDomain:@(MDNSDiscovery::kServiceDomain)
                                         type:@(MDNSDiscovery::kServiceType)
                                         name:@(instanceName.c_str())
                                         port:g_settings.tcpPort()];
    [service scheduleInRunLoop:runLoop forMode:NSRunLoopCommonModes];
    [service setDelegate:self];
    [service setTXTRecordData:MDNSDiscovery::BuildTxtRecordData(txt)];
    [service publish];

    self.publishedService = service;
}

- (void)unpublish {
    if (self.publishedService) {
        [self.publishedService stop];
        self.publishedService.delegate = nil;
        self.publishedService = nil;
    }
    self.publishedNameLower = nil;
    {
        auto& s = MDNSDiscovery::GlobalState();
        s.publishedInstanceName.clear();
    }
}

- (void)startContinuousPublish:(BOOL)publishLocal {
    self.stopRequested = NO;
    __weak ClippMDNSAppleCoordinator* weakSelf = self;
    self.thread = [[NSThread alloc] initWithBlock:^{
        @autoreleasepool {
            ClippMDNSAppleCoordinator* strongSelf = weakSelf;
            if (!strongSelf) return;
            [strongSelf setupBrowserAndPublishOnCurrentRunLoop:publishLocal];
            NSRunLoop* runLoop = [NSRunLoop currentRunLoop];
            while (!strongSelf.stopRequested) {
                @autoreleasepool {
                    [runLoop runMode:NSDefaultRunLoopMode
                          beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
                }
            }
            [strongSelf tearDownOnRunLoop:runLoop];
        }
    }];
    [self.thread setName:@"ClippMDNSDiscovery"];
    [self.thread start];
}

- (void)stopContinuous {
    self.stopRequested = YES;
    // Wait for thread to actually exit (up to 1s).
    for (int i = 0; i < 20 && self.thread && !self.thread.isFinished; ++i) {
        [NSThread sleepForTimeInterval:0.05];
    }
    self.thread = nil;
}

- (void)tearDownOnRunLoop:(NSRunLoop*)runLoop {
    if (self.browser) {
        [self.browser stop];
        [self.browser removeFromRunLoop:runLoop forMode:NSRunLoopCommonModes];
        self.browser.delegate = nil;
        self.browser = nil;
    }
    [self unpublish];
    for (NSNetService* svc in [self.resolving copy]) {
        [svc stop];
        svc.delegate = nil;
    }
    [self.resolving removeAllObjects];
}

#pragma mark - NSNetServiceBrowserDelegate

- (void)netServiceBrowser:(NSNetServiceBrowser*)browser
            didFindService:(NSNetService*)service
                moreComing:(BOOL)moreComing {
    (void)browser; (void)moreComing;

    // Self-filter by instance name (leaf label, case-insensitive).
    NSString* nameLower = [service.name lowercaseString];
    if (self.publishedNameLower && [nameLower isEqualToString:self.publishedNameLower]) {
        return;
    }

    if (self.liveInstances && self.liveInstancesMutex) {
        std::lock_guard<std::mutex> lock(*self.liveInstancesMutex);
        self.liveInstances->try_emplace(LowerCaseUtf8FromNSString(service.name));
    }

    [self.resolving addObject:service];
    service.delegate = self;
    [service scheduleInRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
    [service resolveWithTimeout:5.0];
}

- (void)netServiceBrowser:(NSNetServiceBrowser*)browser
          didRemoveService:(NSNetService*)service
                moreComing:(BOOL)moreComing {
    (void)browser; (void)moreComing;

    auto& s = MDNSDiscovery::GlobalState();
    MDNSDiscovery::Callback cb = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        cb = s.callback;
    }

    ResolvedInstance gone;
    bool wasResolved = false;
    bool fireRemoved = false;
    if (self.liveInstances && self.liveInstancesMutex) {
        const std::string key = LowerCaseUtf8FromNSString(service.name);
        std::lock_guard<std::mutex> lock(*self.liveInstancesMutex);
        auto it = self.liveInstances->find(key);
        if (it != self.liveInstances->end()) {
            gone = it->second;
            wasResolved = gone.resolved;
            self.liveInstances->erase(it);
        }

        // hostId-level dedup: only fire Removed on the non-empty→empty transition.
        if (wasResolved && self.liveByHostId) {
            auto setIt = self.liveByHostId->find(gone.hostId);
            if (setIt != self.liveByHostId->end()) {
                setIt->second.erase(key);
                if (setIt->second.empty()) {
                    self.liveByHostId->erase(setIt);
                    fireRemoved = true;
                }
            }
        }
    }

    if (cb && fireRemoved) {
        MDNSDiscovery::DiscoveredPeer peer;
        peer.deviceName = gone.deviceName;
        peer.hostId = gone.hostId;
        cb(MDNSDiscovery::Event::Removed, peer);
    }
}

#pragma mark - NSNetServiceDelegate (resolve)

- (void)netServiceDidResolveAddress:(NSNetService*)service {
    @autoreleasepool {
        [self handleResolvedService:service];
        [self.resolving removeObject:service];
        service.delegate = nil;
        [service stop];
    }
}

- (void)netService:(NSNetService*)service didNotResolve:(NSDictionary<NSString*, NSNumber*>*)errorDict {
    (void)errorDict;
    [self.resolving removeObject:service];
    service.delegate = nil;
    [service stop];
}

- (void)netService:(NSNetService*)service didNotPublish:(NSDictionary<NSString*, NSNumber*>*)errorDict {
    NSInteger code = [errorDict[NSNetServicesErrorCode] integerValue];
    g_logger.log("MDNSDiscovery::publish", Logger::Level::Warning,
        "DNS-SD: didNotPublish, error=%ld", static_cast<long>(code));
    (void)service;
}

- (void)netServiceDidPublish:(NSNetService*)service {
    // The OS may have renamed us due to conflict — update our self-filter to match.
    auto& s = MDNSDiscovery::GlobalState();
    const char* utf8 = service.name.UTF8String;
    if (utf8) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.publishedInstanceName = MDNSDiscovery::LowerAscii(std::string(utf8));
        self.publishedNameLower = [service.name lowercaseString];
    }
    g_logger.log("MDNSDiscovery::publish", Logger::Level::Info,
        "DNS-SD published as '%s'", utf8 ? utf8 : "?");
}

#pragma mark - Resolve handler

- (void)handleResolvedService:(NSNetService*)service {
    NSData* txtData = service.TXTRecordData;
    if (!txtData) return;
    std::map<std::string, std::string> txt = MDNSDiscovery::ParseTxtRecordData(txtData);

    MDNSProtocol::PacketV1 packet;
    if (!MDNSProtocol::DecodeTxt(txt, packet)) {
        g_logger.log("MDNSDiscovery::resolve", Logger::Level::DDebug,
            "DNS-SD: ignoring '%s' (TXT did not decrypt).",
            service.name.UTF8String ? service.name.UTF8String : "?");
        return;
    }

    auto& s = MDNSDiscovery::GlobalState();
    MDNSDiscovery::Callback cb = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        cb = s.callback;
        const HostId remote(packet.hostId);
        if (remote == s.localHostId && !s.includeSelf) {
            // Same hostId. If the instance name differs from ours, it's a collision.
            NSString* nameLower = [service.name lowercaseString];
            if (self.publishedNameLower && ![nameLower isEqualToString:self.publishedNameLower]) {
                s.hostIDCollisionWarning.store(true);
                g_logger.log("MDNSDiscovery::resolve", Logger::Level::Warning,
                    "DNS-SD: possible host ID collision (peer '%s' shares our host ID).",
                    service.name.UTF8String ? service.name.UTF8String : "?");
            }
            return;
        }
    }

    MDNSDiscovery::DiscoveredPeer peer;
    peer.deviceName = std::string(packet.deviceName);
    peer.hostId = HostId(packet.hostId);
    peer.port = static_cast<uint16_t>(service.port);
    peer.osType = static_cast<OsType>(ntohs(packet.osType));
    std::memcpy(peer.caps, packet.caps, sizeof(peer.caps));

    // Pick the first IPv4 address from service.addresses; fall back to IPv6.
    for (NSData* addrData in service.addresses) {
        const struct sockaddr* sa = static_cast<const struct sockaddr*>(addrData.bytes);
        char ipBuf[INET6_ADDRSTRLEN] = {};
        if (sa->sa_family == AF_INET) {
            const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(sa);
            if (inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf))) {
                peer.ip = ipBuf;
                break;
            }
        } else if (sa->sa_family == AF_INET6) {
            if (peer.ip.empty()) {
                const struct sockaddr_in6* sin6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
                if (inet_ntop(AF_INET6, &sin6->sin6_addr, ipBuf, sizeof(ipBuf))) {
                    peer.ip = ipBuf;
                }
            }
        }
    }
    if (peer.ip.empty()) return;

    bool firstForHost = true;
    if (self.liveInstances && self.liveInstancesMutex) {
        const std::string nameKey = LowerCaseUtf8FromNSString(service.name);
        std::lock_guard<std::mutex> lock(*self.liveInstancesMutex);
        auto& entry = (*self.liveInstances)[nameKey];
        entry.resolved = true;
        entry.hostId = peer.hostId;
        entry.deviceName = peer.deviceName;

        // hostId-level dedup: only fire Added on the empty→non-empty transition.
        if (self.liveByHostId) {
            auto& instancesForHost = (*self.liveByHostId)[peer.hostId];
            firstForHost = instancesForHost.empty();
            instancesForHost.insert(nameKey);
        }
    }

    if (cb && firstForHost) cb(MDNSDiscovery::Event::Added, peer);

    if (self.captureVector && self.captureMutex) {
        std::lock_guard<std::mutex> lock(*self.captureMutex);
        for (const auto& existing : *self.captureVector) {
            if (existing.hostId == peer.hostId) return;
        }
        self.captureVector->push_back(peer);
    }
}

@end

namespace MDNSDiscovery {

namespace {

// One coordinator for continuous Start/Stop; transient one for BrowseOnce.
ClippMDNSAppleCoordinator* gContinuous = nil;

} // namespace

bool Start(Callback callback, bool publishLocal, bool includeSelf) {
    auto& s = GlobalState();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.started) return false;
        s.callback = callback;
        s.publishLocal = publishLocal;
        s.includeSelf = includeSelf;
        s.hostIDCollisionWarning.store(false);
        // Cache our own hostId for self-filtering, even in browse-only mode (share extension).
        g_settings.getHostID(s.localHostId);
        s.started = true;
    }

    gContinuous = [[ClippMDNSAppleCoordinator alloc] init];
    gContinuous.liveInstances = &s.liveInstances;
    gContinuous.liveByHostId = &s.liveByHostId;
    gContinuous.liveInstancesMutex = &s.liveInstancesMutex;
    [gContinuous startContinuousPublish:(publishLocal ? YES : NO)];
    g_logger.log(__FUNCTION__, Logger::Level::Info, "DNS-SD discovery started.");
    return true;
}

void Stop() {
    auto& s = GlobalState();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started) return;
        s.callback = nullptr;
        s.started = false;
    }
    if (gContinuous) {
        [gContinuous stopContinuous];
        gContinuous.liveInstances = nullptr;
        gContinuous.liveByHostId = nullptr;
        gContinuous.liveInstancesMutex = nullptr;
        gContinuous = nil;
    }
    {
        std::lock_guard<std::mutex> lock(s.liveInstancesMutex);
        s.liveInstances.clear();
        s.liveByHostId.clear();
    }
    g_logger.log(__FUNCTION__, Logger::Level::Info, "DNS-SD discovery stopped.");
}

bool HasHostIDCollisionWarning() {
    return GlobalState().hostIDCollisionWarning.load();
}

void ClearHostIDCollisionWarning() {
    GlobalState().hostIDCollisionWarning.store(false);
}

bool BrowseOnce(std::chrono::milliseconds wait, std::vector<DiscoveredPeer>& outPeers, bool includeSelf) {
    outPeers.clear();
    {
        std::lock_guard<std::mutex> lock(GlobalState().mutex);
        if (GlobalState().started) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning,
                "BrowseOnce called while continuous discovery is active; ignoring.");
            return false;
        }
    }

    // Cache our hostId so handleResolvedService can self-filter even though we're not
    // publishing; includeSelf lets the CLI verbs surface the local GUI (same hostId).
    {
        std::lock_guard<std::mutex> lock(GlobalState().mutex);
        g_settings.getHostID(GlobalState().localHostId);
        GlobalState().includeSelf = includeSelf;
    }

    @autoreleasepool {
        ClippMDNSAppleCoordinator* coordinator = [[ClippMDNSAppleCoordinator alloc] init];
        std::mutex captureMutex;
        std::vector<DiscoveredPeer> captured;
        std::mutex localInstancesMutex;
        std::map<std::string, ResolvedInstance> localInstances;
        std::map<HostId, std::set<std::string>> localByHostId;
        coordinator.captureVector = &captured;
        coordinator.captureMutex = &captureMutex;
        coordinator.liveInstances = &localInstances;
        coordinator.liveByHostId = &localByHostId;
        coordinator.liveInstancesMutex = &localInstancesMutex;

        NSRunLoop* runLoop = [NSRunLoop currentRunLoop];
        [coordinator setupBrowserAndPublishOnCurrentRunLoop:NO];

        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:wait.count() / 1000.0];
        while ([deadline timeIntervalSinceNow] > 0) {
            @autoreleasepool {
                [runLoop runMode:NSDefaultRunLoopMode beforeDate:deadline];
            }
        }

        [coordinator tearDownOnRunLoop:runLoop];
        coordinator.captureVector = nullptr;
        coordinator.captureMutex = nullptr;

        std::lock_guard<std::mutex> lock(captureMutex);
        outPeers = std::move(captured);
    }
    {
        std::lock_guard<std::mutex> lock(GlobalState().mutex);
        GlobalState().includeSelf = false;
    }
    return true;
}

} // namespace MDNSDiscovery

#endif // __APPLE__
