#include "ClipboardWire.h"

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
        return false;
    }

    const uint32_t encodedDataSize = static_cast<uint32_t>(payload.rawData.size());
    uint32_t decodedDataSize = payload.decodedDataSize;
    if (decodedDataSize == 0 && !payload.isCompressed) {
        decodedDataSize = encodedDataSize;
    }

    NetworkDefs::ClipboardMessage message{};
    message.formatId = htonl(payload.formatId);
    message.isCompressed = payload.isCompressed ? 1 : 0;
    message.encodedDataSize = htonl(encodedDataSize);
    message.decodedDataSize = htonl(decodedDataSize);

    if (io.socket == INVALID_SOCKET || !channel.SendTaggedMessage(io, "CLIP")) {
        return false;
    }
    if (!channel.SendMessage(io, reinterpret_cast<unsigned char*>(&message), sizeof(message))) {
        return false;
    }
    if (!payload.rawData.empty()
        && !channel.SendMessage(io, payload.rawData.data(), encodedDataSize)) {
        return false;
    }

    if (bytesSent != nullptr) {
        *bytesSent = 4 + sizeof(NetworkDefs::ClipboardMessage) + payload.rawData.size();
    }
    return true;
}

}
