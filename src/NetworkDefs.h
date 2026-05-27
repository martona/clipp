#pragma once

#include "CryptoChannel.h"
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

// Bits for ClipboardMessage::flags.
// CLPM_FLAG_SYNC_REPLAY marks a clipboard message that's being replayed as part
// of activity-stream catch-up after a peer reconnects. Receivers insert it into
// the activity store but do NOT write it to the OS clipboard and do NOT update
// the per-origin "is current" hash guard — these are historical events, not the
// current clipboard contents.
constexpr uint32_t CLPM_FLAG_SYNC_REPLAY = 0x00000001u;

#pragma pack(push, 1)
struct ClipboardMessage {
    // Per-message metadata flags (CLPM_FLAG_*).
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
    // sender so SYNC replay (where the connection partner is a relayer, not the
    // origin) shows the right device name. originHostName is the origin's display
    // name (UTF-8, NUL-padded/truncated). Stamped at origin in StampOrigin.
    uint8_t  originHostId[16];
    char     originHostName[CryptoChannel::HOSTNAME_MAX_BYTES];
    // Sender-assigned counter, monotonic per origin, persisted across restarts.
    uint64_t originSequenceNumber;
    // Random 128-bit per-event identifier. Used for dedup in the activity stream
    // and for "give me everything since X" sync replay queries. A hash isn't
    // enough because identical content copied twice collides; this is unique.
    uint8_t  eventGuid[16];
    uint8_t  reserved1[16];
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

static_assert(sizeof(ClipboardMessage) == 157 + CryptoChannel::HOSTNAME_MAX_BYTES,
    "ClipboardMessage wire layout must stay 157 bytes + HOSTNAME_MAX_BYTES");

// In-memory representation is always native byte order; the wire is always big-endian.
// HtoN / NtoH swap the multi-byte integer fields in place. Both functions are inverses
// of themselves on LE hosts, so the same routine serves both directions.
inline void SwapClipboardMessageByteOrder(ClipboardMessage& msg) {
    msg.flags = htonl(msg.flags);
    msg.timestamp = hton64(msg.timestamp);
    // hashAlg, reserved0, hashBytes, originHostId, originHostName — byte sequences, no swap.
    msg.originSequenceNumber = hton64(msg.originSequenceNumber);
    // eventGuid, reserved1 — byte sequences, no swap.
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
