#include "ClipboardWire.h"

#include "Logger.h"

#include <sodium.h>
#include <xxhash.h>

#include <cstdint>
#include <cstring>

namespace ClipboardWire {

namespace {
constexpr size_t kFrameOverhead = 4 /* tag */
    + sizeof(NetworkDefs::ClipboardMessage)
    + crypto_secretstream_xchacha20poly1305_ABYTES;
constexpr size_t kFrameLimitBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxEncodedPayloadBytes = kFrameLimitBytes - kFrameOverhead;
}

void FinalizeOutgoingPayload(ClipboardPayload& payload, const HostId& originHostId) {
    // hashAlg 1 = XXH3_128 canonical big-endian, first 16 bytes of hashBytes; the
    // trailing 48 bytes stay zero. Field is sized for up to 512-bit future schemes.
    payload.meta.hashAlg = 1;
    const XXH128_hash_t hash = XXH3_128bits_withSeed(
        payload.rawData.data(),
        payload.rawData.size(),
        static_cast<XXH64_hash_t>(payload.meta.formatId));
    XXH128_canonical_t canonical{};
    XXH128_canonicalFromHash(&canonical, hash);
    static_assert(sizeof(canonical.digest) == 16, "XXH128 canonical digest must be 16 bytes");
    std::memset(payload.meta.hashBytes, 0, sizeof(payload.meta.hashBytes));
    std::memcpy(payload.meta.hashBytes, canonical.digest, sizeof(canonical.digest));

    std::memcpy(payload.meta.originHostId, originHostId.data().data(), sizeof(payload.meta.originHostId));

    // payloadDataSize / uncompressedDataSize / isCompressed / formatId are populated
    // by the caller (typically by ZstdCompress just before this call). flags,
    // timestamp, originSequenceNumber, reserved bytes stay at zero.
}

bool SendClipboardPayload(CryptoChannel& channel,
                          const SocketIoContext& io,
                          const ClipboardPayload& payload,
                          std::size_t* bytesSent)
{
    if (payload.rawData.size() > kMaxEncodedPayloadBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Refusing to send clipboard payload: format %ls (%u), payload size %zu bytes exceeds limit %zu bytes",
            ClippClipboardFormatNameW(payload.meta.formatId),
            payload.meta.formatId,
            payload.rawData.size(),
            kMaxEncodedPayloadBytes);
        return false;
    }

    if (io.socket == INVALID_SOCKET) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot send clipboard payload: socket is invalid.");
        return false;
    }

    // Copy the meta into a transient wire-form buffer; never mutate the caller's payload.
    NetworkDefs::ClipboardMessage netMsg = payload.meta;
    NetworkDefs::HostToNetworkClipboardMessage(netMsg);

    if (!channel.SendFrame(io, "CLIP",
                           reinterpret_cast<const unsigned char*>(&netMsg), sizeof(netMsg),
                           payload.rawData.data(), static_cast<uint32_t>(payload.rawData.size()))) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Failed to send CLIP frame for format %ls (%u), payload size %zu bytes.",
            ClippClipboardFormatNameW(payload.meta.formatId),
            payload.meta.formatId,
            payload.rawData.size());
        return false;
    }

    if (bytesSent != nullptr) {
        *bytesSent = 4 + sizeof(netMsg) + payload.rawData.size();
    }
    return true;
}

}
