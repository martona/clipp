#pragma once

#include "platform.h"

#include <cstring>
#include <string>

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR) && defined(__OBJC__)
#import <UIKit/UIKit.h>
#endif

namespace clipp {
namespace local_peer_name_detail {

inline std::string TruncateForCString(std::string value, std::size_t maxBytes) {
    if (maxBytes == 0) {
        return {};
    }
    if (value.size() < maxBytes) {
        return value;
    }

    std::size_t limit = maxBytes - 1;
    while (limit > 0 && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80) {
        --limit;
    }
    if (limit == 0) {
        limit = maxBytes - 1;
    }
    value.resize(limit);
    return value;
}

inline std::string PosixHostName(const char* fallback, std::size_t maxBytes) {
    char hostName[256] = {};
    if (gethostname(hostName, sizeof(hostName)) == 0 && hostName[0] != '\0') {
        return TruncateForCString(hostName, maxBytes);
    }
    return TruncateForCString(fallback != nullptr ? fallback : "unknown", maxBytes);
}

} // namespace local_peer_name_detail

inline std::string GetLocalPeerDisplayName(const char* fallback = "unknown", std::size_t maxBytes = 256) {
#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR) && defined(__OBJC__)
    NSString* deviceName = UIDevice.currentDevice.name;
    if (deviceName.length > 0) {
        const char* utf8 = deviceName.UTF8String;
        if (utf8 != nullptr && utf8[0] != '\0') {
            return local_peer_name_detail::TruncateForCString(utf8, maxBytes);
        }
    }
    return local_peer_name_detail::TruncateForCString(fallback != nullptr ? fallback : "iPhone", maxBytes);
#else
    return local_peer_name_detail::PosixHostName(fallback, maxBytes);
#endif
}

inline bool CopyLocalPeerDisplayName(char* buffer, std::size_t bufferSize, const char* fallback = "unknown") {
    if (buffer == nullptr || bufferSize == 0) {
        return false;
    }

    const std::string name = GetLocalPeerDisplayName(fallback, bufferSize);
    const std::size_t copySize = (std::min)(name.size(), bufferSize - 1);
    std::memcpy(buffer, name.data(), copySize);
    buffer[copySize] = '\0';
    return copySize > 0;
}

} // namespace clipp
