#pragma once

#include "HostId.h"
#include "platform.h"

#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

namespace NetworkDefs {

// All Clipp targets (x64 Windows, x64/arm64 macOS, arm64 iOS) are little-endian.
// If that ever changes, add a __BYTE_ORDER__ check and make these identity on BE.
inline uint64_t hton64(uint64_t v) {
#if defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

inline uint64_t ntoh64(uint64_t v) {
    return hton64(v);
}

#pragma pack(push, 1)
struct ClipboardMessage {
    // Reserved for per-message metadata flags. Zero today.
    uint32_t flags;
    // Origin device's wall-clock at copy time, ms since Unix epoch. 0 = unset.
    // Advisory only — clock drift means it isn't a security primitive.
    uint64_t timestamp;
    // 0 = no hash provided, 1 = XXH3_128 (canonical big-endian, 16 bytes in hashBytes,
    // trailing 48 bytes zero-padded). Field allows up to 512-bit hashes later.
    uint8_t  hashAlg;
    uint8_t  reserved0[3];
    uint8_t  hashBytes[64];
    // The device that originally copied the payload. Distinguished from the immediate
    // sender, in anticipation of relayed/multi-hop transport. Today they match.
    uint8_t  originHostId[16];
    // Sender-assigned counter, monotonic per origin. Zero today; reserved for dedup.
    uint64_t originSequenceNumber;
    uint8_t  reserved1[32];
    // CLIPP_FORMAT_* value. Older peers use the same numeric IDs for UTF-8 and PNG,
    // so these values must remain stable.
    uint32_t formatId;
    // zstd compression flag for the payload bytes that follow this header on the wire.
    uint8_t  isCompressed;
    // Bytes that follow this header on the wire (post-zstd if isCompressed).
    uint64_t payloadDataSize;
    // Post-zstd uncompressed payload size. Not a decoded media/image size.
    uint64_t uncompressedDataSize;
};
#pragma pack(pop)

static_assert(sizeof(ClipboardMessage) == 157, "ClipboardMessage wire layout must stay 157 bytes");

// In-memory representation is always native byte order; the wire is always big-endian.
// HtoN / NtoH swap the multi-byte integer fields in place. Both functions are inverses
// of themselves on LE hosts, so the same routine serves both directions.
inline void SwapClipboardMessageByteOrder(ClipboardMessage& msg) {
    msg.flags = htonl(msg.flags);
    msg.timestamp = hton64(msg.timestamp);
    // hashAlg, reserved0, hashBytes, originHostId — byte sequences, no swap.
    msg.originSequenceNumber = hton64(msg.originSequenceNumber);
    // reserved1 — byte sequence, no swap.
    msg.formatId = htonl(msg.formatId);
    // isCompressed — single byte, no swap.
    msg.payloadDataSize = hton64(msg.payloadDataSize);
    msg.uncompressedDataSize = hton64(msg.uncompressedDataSize);
}

inline void HostToNetworkClipboardMessage(ClipboardMessage& msg) {
    SwapClipboardMessageByteOrder(msg);
}

inline void NetworkToHostClipboardMessage(ClipboardMessage& msg) {
    SwapClipboardMessageByteOrder(msg);
}

}
