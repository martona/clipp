#pragma once

#include "ClipboardData.h"
#include "CryptoChannel.h"
#include "utils_socket.h"

#include <cstddef>

namespace ClipboardWire {

bool SendClipboardPayload(CryptoChannel& channel,
                          const SocketIoContext& io,
                          const ClipboardPayload& payload,
                          std::size_t* bytesSent = nullptr);

}
