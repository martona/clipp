#pragma once

#include "ClipboardPayload.h"
#include "CryptoChannel.h"
#include "HostId.h"
#include "utils_socket.h"

#include <cstddef>
#include <vector>

namespace ClipboardWire {

bool SendClipboardPayload(CryptoChannel& channel,
                          const SocketIoContext& io,
                          const ClipboardPayload& payload,
                          std::size_t* bytesSent = nullptr);

// Decodes a received CLIP frame (4-byte tag + ClipboardMessage header + body) into
// `out`, validating the header/body sizes, the uncompressed-size limit, and the
// uncompressed payload-size invariant. The leading 4-byte tag is assumed already
// matched by the caller. Returns false (after logging the reason) on a malformed
// frame, leaving `out` in an unspecified state.
bool TryDecodeClipboardFrame(const std::vector<unsigned char>& frame, ClipboardPayload& out);

}
