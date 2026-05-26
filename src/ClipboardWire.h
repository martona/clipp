#pragma once

#include "ClipboardPayload.h"
#include "CryptoChannel.h"
#include "HostId.h"
#include "utils_socket.h"

#include <cstddef>

namespace ClipboardWire {

// Fills wire-only metadata into payload.meta (hash, hashAlg, originHostId).
// Format/compression/sizes must already be populated by the caller — typically via
// ZstdCompress, which is the right "ready-to-send" state. Flags / timestamp /
// originSequenceNumber stay at zero today; see project memory.
void FinalizeOutgoingPayload(ClipboardPayload& payload, const HostId& originHostId);

bool SendClipboardPayload(CryptoChannel& channel,
                          const SocketIoContext& io,
                          const ClipboardPayload& payload,
                          std::size_t* bytesSent = nullptr);

}
