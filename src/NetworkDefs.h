#pragma once

#include "HostId.h"

#include <cstdint>

namespace NetworkDefs {
#pragma pack(push, 1)
struct ClientHello {
	wchar_t selector[8];
	unsigned short version;
	unsigned char hostID[HostId::kSize];
	wchar_t hostName[256];
};

struct ClipboardMessage {
	// CLIPP_FORMAT_* value. Older peers use the same numeric IDs for UTF-8
	// and PNG, so these values must remain stable.
	uint32_t formatId;
	// zstd compression flag for the payload bytes that follow this header.
	uint8_t isCompressed;
	uint32_t payloadDataSize;
	// Historical packet slot formerly called decodedDataSize. It is the
	// post-zstd payload size, not a decoded media/image size.
	uint32_t uncompressedDataSize;
};
#pragma pack(pop)

constexpr const wchar_t* kSelector = L"clipp";
constexpr unsigned short kVersion = 2;
}
