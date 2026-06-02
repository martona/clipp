#include "platform.h"

#include "NetworkInterfaces.h"

#include <algorithm>
#include <string>
#include <vector>

#include <xxhash.h>

#ifdef _WIN32
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#elif defined(__APPLE__) || defined(__linux__)
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace clipp {

namespace {

uint64_t HashTokens(std::vector<std::string>& tokens) {
    if (tokens.empty()) {
        // Empty is a legitimate, stable state (e.g., everything down) and must hash to
        // something distinct from the 0 "enumeration failed" sentinel.
        static const char kEmpty[] = "clipp-no-interfaces";
        return XXH3_64bits(kEmpty, sizeof(kEmpty) - 1);
    }
    std::sort(tokens.begin(), tokens.end());
    std::string joined;
    for (const std::string& t : tokens) {
        joined += t;
        joined.push_back('\n');
    }
    return XXH3_64bits(joined.data(), joined.size());
}

}  // namespace

#ifdef _WIN32

uint64_t HashLocalInterfaceAddresses() {
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
    ULONG size = 15000;
    std::vector<unsigned char> buffer(size);
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) {
        return 0;
    }

    std::vector<std::string> tokens;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const sockaddr* sa = unicast->Address.lpSockaddr;
            if (sa == nullptr) continue;
            if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) continue;

            char ip[INET6_ADDRSTRLEN] = {};
            if (sa->sa_family == AF_INET) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
                if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
            } else {
                const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(sa);
                if (!inet_ntop(AF_INET6, &sin6->sin6_addr, ip, sizeof(ip))) continue;
            }

            // IfIndex disambiguates link-local addresses that repeat across adapters.
            std::string token = std::to_string(adapter->IfIndex);
            token.push_back('|');
            token += std::to_string(sa->sa_family);
            token.push_back('|');
            token += ip;
            tokens.push_back(std::move(token));
        }
    }
    return HashTokens(tokens);
}

#elif defined(__APPLE__) || defined(__linux__)

uint64_t HashLocalInterfaceAddresses() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0 || ifaddr == nullptr) {
        return 0;
    }

    std::vector<std::string> tokens;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        const int family = ifa->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (!(ifa->ifa_flags & IFF_RUNNING)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        char ip[INET6_ADDRSTRLEN] = {};
        if (family == AF_INET) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
            if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
        } else {
            const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(ifa->ifa_addr);
            if (!inet_ntop(AF_INET6, &sin6->sin6_addr, ip, sizeof(ip))) continue;
        }

        // Interface name tags the address so a same-IP move between interfaces still
        // changes the hash, and so IPv6 link-local addresses (which repeat per
        // interface) stay distinct.
        std::string token = ifa->ifa_name ? ifa->ifa_name : "?";
        token.push_back('|');
        token += std::to_string(family);
        token.push_back('|');
        token += ip;
        tokens.push_back(std::move(token));
    }

    freeifaddrs(ifaddr);
    return HashTokens(tokens);
}

#else

uint64_t HashLocalInterfaceAddresses() { return 0; }

#endif

}  // namespace clipp
