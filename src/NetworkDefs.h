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
	uint32_t formatId;
	uint8_t isCompressed;
	uint32_t encodedDataSize;
	uint32_t decodedDataSize;
};
#pragma pack(pop)

constexpr const wchar_t* kSelector = L"clipp";
constexpr unsigned short kVersion = 2;
}
