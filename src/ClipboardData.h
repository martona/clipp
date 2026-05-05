#pragma once
#include <cstdlib>
#include <vector>
#include <zstd.h>
#include "ClipboardLimits.h"
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

		const unsigned long long decompressedSize = ZSTD_getFrameContentSize(rawData.data(), rawData.size());
		if (decompressedSize == ZSTD_CONTENTSIZE_ERROR) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: invalid zstd frame (compressed size: %zu bytes)", rawData.size());
			return false;
		}

		if (decompressedSize == ZSTD_CONTENTSIZE_UNKNOWN) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: unknown decompressed size (compressed size: %zu bytes)", rawData.size());
			return false;
		}

		if (decompressedSize > ClipboardLimits::kMaxDecompressedClipboardBytes) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: decompressed size %llu bytes exceeds limit %llu bytes", decompressedSize, ClipboardLimits::kMaxDecompressedClipboardBytes);
			return false;
		}

		std::vector<unsigned char> decompressedData(static_cast<size_t>(decompressedSize));
		const size_t actualSize = ZSTD_decompress(
			decompressedData.data(),
			decompressedData.size(),
			rawData.data(),
			rawData.size());

		if (ZSTD_isError(actualSize) != 0) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: zstd decompression failed (%hs)", ZSTD_getErrorName(actualSize));
			return false;
		}

		if (actualSize != decompressedSize) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: decompressed size mismatch (expected %llu bytes, actual %zu bytes)", decompressedSize, actualSize);
			return false;
		}

		g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Decompressed data from %zu bytes to %zu bytes", rawData.size(), actualSize);
		rawData = std::move(decompressedData);
		isCompressed = false;
		return true;
	}
};
