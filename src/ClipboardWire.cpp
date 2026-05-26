#include "ClipboardWire.h"

#include "Logger.h"

#include <sodium.h>

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
    // Hash / hashAlg / sizes / isCompressed are filled by SetUncompressedBytes.
    // This call only stamps the origin identity — and later, timestamp /
    // sequence number when those go live.
    std::memcpy(payload.meta.originHostId,
                originHostId.data().data(),
                sizeof(payload.meta.originHostId));
}

bool SendClipboardPayload(CryptoChannel& channel,
                          const SocketIoContext& io,
                          const ClipboardPayload& payload,
                          std::size_t* bytesSent)
{
    const std::vector<unsigned char>& body = payload.EncodedBytes();

    if (body.size() > kMaxEncodedPayloadBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Refusing to send clipboard payload: format %ls (%u), payload size %zu bytes exceeds limit %zu bytes",
            ClippClipboardFormatNameW(payload.meta.formatId),
            payload.meta.formatId,
            body.size(),
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
                           body.data(), static_cast<uint32_t>(body.size()))) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Failed to send CLIP frame for format %ls (%u), payload size %zu bytes.",
            ClippClipboardFormatNameW(payload.meta.formatId),
            payload.meta.formatId,
            body.size());
        return false;
    }

    if (bytesSent != nullptr) {
        *bytesSent = 4 + sizeof(netMsg) + body.size();
    }
    return true;
}

}
