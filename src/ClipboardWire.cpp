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

bool TryDecodeClipboardFrame(const std::vector<unsigned char>& frame, ClipboardPayload& out)
{
    constexpr size_t kClipHeaderSize = sizeof(NetworkDefs::ClipboardMessage);
    if (frame.size() < 4 + kClipHeaderSize) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Rejecting CLIP frame: too small for header (frame: %zu bytes, header: %zu bytes).",
            frame.size(), kClipHeaderSize);
        return false;
    }

    out = ClipboardPayload{};
    std::memcpy(&out.meta, frame.data() + 4, kClipHeaderSize);
    NetworkDefs::NetworkToHostClipboardMessage(out.meta);

    const size_t expectedBodyBytes = frame.size() - 4 - kClipHeaderSize;
    if (out.meta.payloadDataSize != static_cast<uint64_t>(expectedBodyBytes)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Rejecting CLIP frame: payload size mismatch (header: %llu bytes, body: %zu bytes).",
            static_cast<unsigned long long>(out.meta.payloadDataSize), expectedBodyBytes);
        return false;
    }

    if (out.meta.uncompressedDataSize > ClipboardLimits::kMaxDecompressedClipboardBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Rejecting CLIP frame: uncompressed size %llu bytes exceeds limit %llu bytes.",
            static_cast<unsigned long long>(out.meta.uncompressedDataSize),
            ClipboardLimits::kMaxDecompressedClipboardBytes);
        return false;
    }

    if (out.meta.isCompressed == 0
        && out.meta.payloadDataSize != out.meta.uncompressedDataSize) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Rejecting uncompressed CLIP frame: payload size %llu bytes does not equal uncompressed size %llu bytes.",
            static_cast<unsigned long long>(out.meta.payloadDataSize),
            static_cast<unsigned long long>(out.meta.uncompressedDataSize));
        return false;
    }

    std::vector<unsigned char> body;
    if (expectedBodyBytes > 0) {
        body.assign(
            frame.data() + 4 + kClipHeaderSize,
            frame.data() + 4 + kClipHeaderSize + expectedBodyBytes);
    }
    out.SetEncodedBytes(std::move(body));
    return true;
}

}
