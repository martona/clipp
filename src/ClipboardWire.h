#pragma once

#include "ClipboardPayload.h"
#include "CryptoChannel.h"
#include "HostId.h"
#include "utils_socket.h"

#include <cstddef>

namespace ClipboardWire {

bool SendClipboardPayload(CryptoChannel& channel,
                          const SocketIoContext& io,
                          const ClipboardPayload& payload,
                          std::size_t* bytesSent = nullptr);

}
