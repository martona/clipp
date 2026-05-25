#pragma once
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>
#include <zstd.h>
#include "ClipboardFormat.h"
#include "ClipboardLimits.h"
#include "Logger.h"

struct ClipboardPayload {
	// Clipp wire format (CLIPP_FORMAT_*). Platform code translates native
	// clipboard formats at the OS boundary.
	uint32_t formatId{ CLIPP_FORMAT_NONE };
	// When true, rawData contains a zstd frame. Image payloads are already
	// compressed by their media format, so this is normally text-only.
	bool isCompressed{ false };
	// Size of rawData after zstd decompression. This is not decoded image bitmap size.
	uint32_t uncompressedDataSize{ 0 };
	std::vector<unsigned char> rawData;

	bool ZstdCompress() {
		if (rawData.size() > std::numeric_limits<uint32_t>::max()) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard payload: data size %zu bytes exceeds uint32_t maximum", rawData.size());
			return false;
		}

		uncompressedDataSize = static_cast<uint32_t>(rawData.size());
		if (rawData.size() < 512 || formatId != CLIPP_FORMAT_UTF8) {
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
		const uint32_t expectedUncompressedSize = uncompressedDataSize;
		if (expectedUncompressedSize > ClipboardLimits::kMaxDecompressedClipboardBytes) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard payload: uncompressed size %u bytes exceeds limit %llu bytes", expectedUncompressedSize, ClipboardLimits::kMaxDecompressedClipboardBytes);
			return false;
		}

		if (!isCompressed) {
			if (rawData.size() != expectedUncompressedSize) {
				g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting uncompressed clipboard payload: size mismatch (expected %u bytes, actual %zu bytes)", expectedUncompressedSize, rawData.size());
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

		if (decompressedSize != expectedUncompressedSize) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: frame uncompressed size mismatch (expected %u bytes, frame reports %llu bytes)", expectedUncompressedSize, decompressedSize);
			return false;
		}

		std::vector<unsigned char> decompressedData(static_cast<size_t>(expectedUncompressedSize));
		const size_t actualSize = ZSTD_decompress(
			decompressedData.data(),
			decompressedData.size(),
			rawData.data(),
			rawData.size());

		if (ZSTD_isError(actualSize) != 0) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: zstd decompression failed (%hs)", ZSTD_getErrorName(actualSize));
			return false;
		}

		if (actualSize != expectedUncompressedSize) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: decompressed size mismatch (expected %u bytes, actual %zu bytes)", expectedUncompressedSize, actualSize);
			return false;
		}

		g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Decompressed data from %zu bytes to %zu bytes", rawData.size(), actualSize);
		rawData = std::move(decompressedData);
		isCompressed = false;
		return true;
	}
};
