#pragma once
#include <cstdlib>
#include <vector>
struct ClipboardPayload {
	uint32_t formatId;
	std::vector<unsigned char> rawData;
};
