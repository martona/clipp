#include "ClipboardWire.h"

#include "Logger.h"
#include "NetworkDefs.h"

#include <sodium.h>

#include <cstdint>

namespace ClipboardWire {

bool SendClipboardPayload(CryptoChannel& channel,
                          const SocketIoContext& io,
                          const ClipboardPayload& payload,
                          std::size_t* bytesSent) {
    const size_t maxEncodedPayloadBytes = (64u * 1024u * 1024u)
        - sizeof(NetworkDefs::ClipboardMessage)
        - crypto_secretstream_xchacha20poly1305_ABYTES;
    if (payload.rawData.size() > maxEncodedPayloadBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Refusing to send clipboard payload: format %ls (%u), payload size %zu bytes exceeds limit %zu bytes",
            ClippClipboardFormatNameW(payload.formatId),
            payload.formatId,
            payload.rawData.size(),
            maxEncodedPayloadBytes);
        return false;
    }

    const uint32_t payloadDataSize = static_cast<uint32_t>(payload.rawData.size());
    uint32_t uncompressedDataSize = payload.uncompressedDataSize;
    if (uncompressedDataSize == 0 && !payload.isCompressed) {
        uncompressedDataSize = payloadDataSize;
    }

    NetworkDefs::ClipboardMessage message{};
    message.formatId = htonl(payload.formatId);
    message.isCompressed = payload.isCompressed ? 1 : 0;
    message.payloadDataSize = htonl(payloadDataSize);
    message.uncompressedDataSize = htonl(uncompressedDataSize);

    if (io.socket == INVALID_SOCKET) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot send clipboard payload: socket is invalid.");
        return false;
    }
    if (!channel.SendTaggedMessage(io, "CLIP")) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to send clipboard CLIP tag.");
        return false;
    }
    if (!channel.SendMessage(io, reinterpret_cast<unsigned char*>(&message), sizeof(message))) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to send clipboard header for format %ls (%u).",
            ClippClipboardFormatNameW(payload.formatId),
            payload.formatId);
        return false;
    }
    if (!payload.rawData.empty()
        && !channel.SendMessage(io, payload.rawData.data(), payloadDataSize)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to send clipboard payload body for format %ls (%u), payload size %u bytes.",
            ClippClipboardFormatNameW(payload.formatId),
            payload.formatId,
            payloadDataSize);
        return false;
    }

    if (bytesSent != nullptr) {
        *bytesSent = 4 + sizeof(NetworkDefs::ClipboardMessage) + payload.rawData.size();
    }
    return true;
}

}
