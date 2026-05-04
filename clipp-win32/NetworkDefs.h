#pragma once

#include <cstdint>

namespace NetworkDefs {
#pragma pack(push, 1)
struct ClientHello {
	wchar_t selector[8];
	unsigned short version;
	unsigned char hostID[32];
	wchar_t hostName[256];
};

struct ClipboardMessage {
	uint32_t formatId;
	uint8_t isCompressed;
	uint32_t rawDataSize;
};
#pragma pack(pop)

constexpr const wchar_t* kSelector = L"clipp";
constexpr unsigned short kVersion = 1;
}
