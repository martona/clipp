#pragma once
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>
#include <zstd.h>
#include "ClipboardLimits.h"
#include "Logger.h"

#ifndef CF_UNICODETEXT
	#define CF_UNICODETEXT 13
#endif
#ifndef CF_DIB
	#define CF_DIB 8
#endif

struct ClipboardPayload {
	uint32_t formatId;
	bool isCompressed{ false };
	uint32_t decodedDataSize{ 0 };
	std::vector<unsigned char> rawData;

	bool ZstdCompress() {
		if (rawData.size() > std::numeric_limits<uint32_t>::max()) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard payload: data size %zu bytes exceeds uint32_t maximum", rawData.size());
			return false;
		}

		decodedDataSize = static_cast<uint32_t>(rawData.size());
		if (rawData.size() < 512 || formatId != CF_UNICODETEXT) {
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
		const uint32_t expectedDecodedSize = decodedDataSize;
		if (expectedDecodedSize > ClipboardLimits::kMaxDecompressedClipboardBytes) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard payload: decoded size %u bytes exceeds limit %llu bytes", expectedDecodedSize, ClipboardLimits::kMaxDecompressedClipboardBytes);
			return false;
		}

		if (!isCompressed) {
			if (rawData.size() != expectedDecodedSize) {
				g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting uncompressed clipboard payload: decoded size mismatch (expected %u bytes, actual %zu bytes)", expectedDecodedSize, rawData.size());
				return false;
			}
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

		if (decompressedSize != expectedDecodedSize) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: frame decoded size mismatch (expected %u bytes, frame reports %llu bytes)", expectedDecodedSize, decompressedSize);
			return false;
		}

		std::vector<unsigned char> decompressedData(static_cast<size_t>(expectedDecodedSize));
		const size_t actualSize = ZSTD_decompress(
			decompressedData.data(),
			decompressedData.size(),
			rawData.data(),
			rawData.size());

		if (ZSTD_isError(actualSize) != 0) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: zstd decompression failed (%hs)", ZSTD_getErrorName(actualSize));
			return false;
		}

		if (actualSize != expectedDecodedSize) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: decompressed size mismatch (expected %u bytes, actual %zu bytes)", expectedDecodedSize, actualSize);
			return false;
		}

		g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Decompressed data from %zu bytes to %zu bytes", rawData.size(), actualSize);
		rawData = std::move(decompressedData);
		isCompressed = false;
		return true;
	}
};
