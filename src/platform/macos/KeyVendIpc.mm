#include "KeyVendIpc.h"

#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <bsm/audit.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <sodium.h>

#include "../../KeyManager.h"
#include "../../Logger.h"
#include "../../utils_socket.h"

namespace {

// Wire protocol (request: magic + opcode; response: status [+ key bytes]).
constexpr unsigned char kProtocolMagic[4] = { 'C', 'L', 'P', 'K' };
constexpr unsigned char kOpRequestRootKey = 0x01;
constexpr unsigned char kStatusOk = 0x00;
constexpr unsigned char kStatusNoKey = 0x01;

std::atomic<bool> g_serverActive{ false };
std::atomic<int> g_listenFd{ -1 };
std::thread g_serverThread;
SocketWakeEvent g_wakeEvent;

// --- socket path -------------------------------------------------------------

std::string SocketPath() {
    @autoreleasepool {
        NSString* dir = nil;
        if (getenv("APP_SANDBOX_CONTAINER_ID") != nullptr) {
            // Sandboxed: HOME is the per-app container's Data dir.
            dir = NSHomeDirectory();
        } else {
            NSArray<NSURL*>* urls = [[NSFileManager defaultManager]
                URLsForDirectory:NSApplicationSupportDirectory
                          inDomains:NSUserDomainMask];
            NSURL* base = urls.firstObject;
            if (base == nil) {
                return std::string();
            }
            NSURL* appDir = [base URLByAppendingPathComponent:@"net.clipp.ios" isDirectory:YES];
            [[NSFileManager defaultManager] createDirectoryAtURL:appDir
                                     withIntermediateDirectories:YES
                                                      attributes:nil
                                                           error:nil];
            dir = appDir.path;
        }
        if (dir == nil) {
            return std::string();
        }
        return std::string([dir UTF8String]) + "/keyvend.sock";
    }
}

bool PathFitsSockaddr(const std::string& path) {
    sockaddr_un probe{};
    return !path.empty() && path.size() < sizeof(probe.sun_path);
}

// --- peer authentication -----------------------------------------------------

// This binary's own designated requirement. Since the GUI and CLI are the same
// on-disk executable, requiring a peer to satisfy it pins to "the same signed
// clipp" across MAS / Developer-ID / ad-hoc builds without hardcoding team id.
SecRequirementRef CopySelfDesignatedRequirement() {
    SecCodeRef selfCode = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &selfCode) != errSecSuccess || selfCode == nullptr) {
        return nullptr;
    }
    SecStaticCodeRef staticCode = nullptr;
    OSStatus status = SecCodeCopyStaticCode(selfCode, kSecCSDefaultFlags, &staticCode);
    CFRelease(selfCode);
    if (status != errSecSuccess || staticCode == nullptr) {
        return nullptr;
    }
    SecRequirementRef requirement = nullptr;
    status = SecCodeCopyDesignatedRequirement(staticCode, kSecCSDefaultFlags, &requirement);
    CFRelease(staticCode);
    if (status != errSecSuccess) {
        return nullptr;
    }
    return requirement;
}

// Verify the process on the other end of `fd` is the same signed clipp binary.
bool AuthenticatePeer(int fd) {
    audit_token_t token{};
    socklen_t len = sizeof(token);
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &len) != 0 || len != sizeof(token)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
                     "LOCAL_PEERTOKEN failed (errno=%d); refusing peer.", errno);
        return false;
    }

    SecRequirementRef requirement = CopySelfDesignatedRequirement();
    if (requirement == nullptr) {
        // No usable designated requirement means this binary is unsigned (a fully
        // unsigned dev build). A signed Developer-ID/MAS build always has one, so
        // this branch never fires in production. Fail closed: refuse rather than
        // hand the key to an unverifiable peer. (Dev fix: ad-hoc sign with
        // `codesign -s - clipp.app`, which arm64 already does at link time.)
        g_logger.log(__FUNCTION__, Logger::Level::Error,
                     "No designated requirement (unsigned build?); refusing peer. "
                     "Ad-hoc sign the binary to use the key-vend socket.");
        return false;
    }

    bool authenticated = false;
    CFDataRef tokenData = CFDataCreate(kCFAllocatorDefault,
                                       reinterpret_cast<const UInt8*>(&token), sizeof(token));
    if (tokenData != nullptr) {
        const void* keys[] = { kSecGuestAttributeAudit };
        const void* values[] = { tokenData };
        CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                        &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks);
        if (attributes != nullptr) {
            SecCodeRef peerCode = nullptr;
            OSStatus status = SecCodeCopyGuestWithAttributes(nullptr, attributes,
                                                             kSecCSDefaultFlags, &peerCode);
            if (status == errSecSuccess && peerCode != nullptr) {
                status = SecCodeCheckValidity(peerCode, kSecCSDefaultFlags, requirement);
                authenticated = (status == errSecSuccess);
                if (!authenticated) {
                    g_logger.log(__FUNCTION__, Logger::Level::Warning,
                                 "Peer failed code-signing validity (OSStatus=%d).", (int)status);
                }
                CFRelease(peerCode);
            } else {
                g_logger.log(__FUNCTION__, Logger::Level::Warning,
                             "SecCodeCopyGuestWithAttributes failed (OSStatus=%d).", (int)status);
            }
            CFRelease(attributes);
        }
        CFRelease(tokenData);
    }
    CFRelease(requirement);
    return authenticated;
}

// --- framed blocking I/O -----------------------------------------------------

bool WriteAll(int fd, const void* buffer, size_t count) {
    const unsigned char* p = static_cast<const unsigned char*>(buffer);
    size_t off = 0;
    while (off < count) {
        ssize_t n = write(fd, p + off, count - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

bool ReadAll(int fd, void* buffer, size_t count) {
    unsigned char* p = static_cast<unsigned char*>(buffer);
    size_t off = 0;
    while (off < count) {
        ssize_t n = read(fd, p + off, count - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // peer closed early
        off += static_cast<size_t>(n);
    }
    return true;
}

// --- server ------------------------------------------------------------------

void HandleClient(int clientFd) {
    if (!AuthenticatePeer(clientFd)) {
        close(clientFd);
        return;
    }

    unsigned char request[5]{};
    if (!ReadAll(clientFd, request, sizeof(request)) ||
        std::memcmp(request, kProtocolMagic, sizeof(kProtocolMagic)) != 0 ||
        request[4] != kOpRequestRootKey) {
        close(clientFd);
        return;
    }

    KeyManager::NetworkKey rootKey{};
    std::string error;
    if (!g_keyManager.ExportRootKeyFromKeychain(rootKey, &error)) {
        const unsigned char response = kStatusNoKey;
        WriteAll(clientFd, &response, 1);
        sodium_memzero(rootKey.data(), rootKey.size());
        close(clientFd);
        return;
    }

    unsigned char response[1 + KeyManager::NetworkKeySize]{};
    response[0] = kStatusOk;
    std::memcpy(response + 1, rootKey.data(), rootKey.size());
    WriteAll(clientFd, response, sizeof(response));
    sodium_memzero(rootKey.data(), rootKey.size());
    sodium_memzero(response, sizeof(response));
    close(clientFd);
}

void ServerLoop(int listenFd) {
    // Block in select() on the listen socket plus the wake event; StopKeyVendServer()
    // signals the wake event to break the wait immediately (closing a listening
    // socket from another thread doesn't reliably unblock accept() on macOS). The
    // server only starts once the wake event is initialized, so it's valid here.
    const int wakeFd = static_cast<int>(g_wakeEvent.Socket());
    const int maxFd = std::max(listenFd, wakeFd);
    while (g_serverActive.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenFd, &readSet);
        FD_SET(wakeFd, &readSet);
        const int ready = select(maxFd + 1, &readSet, nullptr, nullptr, nullptr);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (FD_ISSET(wakeFd, &readSet)) {
            g_wakeEvent.Drain();
            continue;  // woken for shutdown; the loop condition re-checks the flag
        }
        if (FD_ISSET(listenFd, &readSet)) {
            int clientFd = accept(listenFd, nullptr, nullptr);
            if (clientFd >= 0) {
                HandleClient(clientFd);
            }
        }
    }
}

}  // namespace

namespace clipp::macos {

void StartKeyVendServer() {
    if (g_serverActive.exchange(true)) {
        return;  // already running
    }

    const std::string path = SocketPath();
    if (!PathFitsSockaddr(path)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error,
                     "Key-vend socket path empty or too long for sun_path: %s", path.c_str());
        g_serverActive.store(false);
        return;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Key-vend socket() failed (errno=%d).", errno);
        g_serverActive.store(false);
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    unlink(path.c_str());  // clear a stale socket from a previous (crashed) run
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Key-vend bind() failed (errno=%d).", errno);
        close(fd);
        g_serverActive.store(false);
        return;
    }
    // Owner-only as defense in depth; the code-signing check is the real gate.
    chmod(path.c_str(), S_IRUSR | S_IWUSR);

    if (listen(fd, 4) != 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, "Key-vend listen() failed (errno=%d).", errno);
        close(fd);
        unlink(path.c_str());
        g_serverActive.store(false);
        return;
    }

    if (!g_wakeEvent.Initialize()) {
        // Can't allocate the wake event -> the system is out of socket resources,
        // so the app is already in trouble. Don't start a server we can't cleanly
        // stop; bail (local keychain access is unaffected).
        g_logger.log(__FUNCTION__, Logger::Level::Error,
                     "Key-vend wake event failed to initialize; not starting the server.");
        close(fd);
        unlink(path.c_str());
        g_serverActive.store(false);
        return;
    }
    g_listenFd.store(fd);
    g_serverThread = std::thread(ServerLoop, fd);
    g_logger.log(__FUNCTION__, Logger::Level::Info, "Key-vend server listening at %s", path.c_str());
}

void StopKeyVendServer() {
    if (!g_serverActive.exchange(false)) {
        return;
    }
    g_wakeEvent.Signal();  // break the select() immediately -- no shutdown lag
    if (g_serverThread.joinable()) {
        g_serverThread.join();
    }
    int fd = g_listenFd.exchange(-1);
    if (fd >= 0) {
        close(fd);
    }
    g_wakeEvent.Close();
    unlink(SocketPath().c_str());
}

bool IsKeyVendServerActive() {
    return g_serverActive.load();
}

bool RequestNetworkKeyOverSocket(KeyManager::NetworkKey& outKey, std::string* errorMessage) {
    const std::string path = SocketPath();
    if (!PathFitsSockaddr(path)) {
        if (errorMessage) *errorMessage = "Key-vend socket path unusable.";
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (errorMessage) *errorMessage = "socket() failed.";
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errorMessage) *errorMessage = "Clipp isn't running (could not connect to the key-vend socket).";
        close(fd);
        return false;
    }

    // Verify the server is the same signed clipp before trusting any key from it.
    if (!AuthenticatePeer(fd)) {
        if (errorMessage) *errorMessage = "Key-vend server failed authentication.";
        close(fd);
        return false;
    }

    unsigned char request[5];
    std::memcpy(request, kProtocolMagic, sizeof(kProtocolMagic));
    request[4] = kOpRequestRootKey;
    if (!WriteAll(fd, request, sizeof(request))) {
        if (errorMessage) *errorMessage = "Failed to send key-vend request.";
        close(fd);
        return false;
    }

    unsigned char status = 0xFF;
    if (!ReadAll(fd, &status, 1)) {
        if (errorMessage) *errorMessage = "Failed to read key-vend response.";
        close(fd);
        return false;
    }
    if (status != kStatusOk) {
        if (errorMessage) *errorMessage = "The running clipp has no network key configured.";
        close(fd);
        return false;
    }

    unsigned char keyBytes[KeyManager::NetworkKeySize]{};
    if (!ReadAll(fd, keyBytes, sizeof(keyBytes))) {
        if (errorMessage) *errorMessage = "Failed to read the network key from the key-vend response.";
        sodium_memzero(keyBytes, sizeof(keyBytes));
        close(fd);
        return false;
    }

    std::memcpy(outKey.data(), keyBytes, KeyManager::NetworkKeySize);
    sodium_memzero(keyBytes, sizeof(keyBytes));
    close(fd);
    return true;
}

}  // namespace clipp::macos
