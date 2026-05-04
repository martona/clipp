#pragma once
#include <cstdlib>
#include <vector>
#include <zstd.h>
#include "Logger.h"

struct ClipboardPayload {
	uint32_t formatId;
	bool isCompressed{ false };
	std::vector<unsigned char> rawData;

	bool ZstdCompress() {
		if (rawData.size() < 512) {
			isCompressed = false;
			return true;
		}

		const size_t bound = ZSTD_compressBound(rawData.size());
		std::vector<unsigned char> compressedData(bound);
		const size_t compressedSize = ZSTD_compress(
			compressedData.data(),
			compressedData.size(),
			rawData.data(),
			rawData.size(),
			ZSTD_fast);

		if (ZSTD_isError(compressedSize) != 0) {
			return false;
		}

		g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Compressed data from %zu bytes to %zu bytes", rawData.size(), compressedSize);
		compressedData.resize(compressedSize);
		rawData = std::move(compressedData);
		isCompressed = true;
		return true;
	}

	bool ZstdDecompress() {
		if (!isCompressed) {
			return true;
		}
		uint64_t decompressedSize = ZSTD_getFrameContentSize(rawData.data(), rawData.size());
		std::vector<unsigned char> decompressedData(decompressedSize);
		const size_t actualSize = ZSTD_decompress(
			decompressedData.data(),
			decompressedData.size(),
			rawData.data(),
			rawData.size());

		if (ZSTD_isError(actualSize) != 0 || actualSize != decompressedSize) {
			return false;
		}

		g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Decompressed data from %zu bytes to %zu bytes", rawData.size(), actualSize);
		rawData = std::move(decompressedData);
		isCompressed = false;
		return true;
	}
};
