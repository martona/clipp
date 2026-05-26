#pragma once
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <zstd.h>
#include "ClipboardFormat.h"
#include "ClipboardLimits.h"
#include "Logger.h"
#include "NetworkDefs.h"

// One in-memory representation of a clipboard item, end to end.
// `meta` is the same struct that travels on the wire (NetworkDefs::ClipboardMessage),
// so format/compression/sizes/hash/origin all live in one place. `rawData` is the
// payload bytes — compressed iff `meta.isCompressed != 0`. The store, the wire layer,
// and the platform clipboard adapters all operate on this single struct.
struct ClipboardPayload {
	NetworkDefs::ClipboardMessage meta{};
	std::vector<unsigned char> rawData;

	// Compresses rawData with zstd when it's worth it (text-only, ≥ 512 bytes).
	// Updates meta.isCompressed, meta.uncompressedDataSize, meta.payloadDataSize.
	bool ZstdCompress() {
		meta.uncompressedDataSize = static_cast<uint64_t>(rawData.size());

		if (rawData.size() < 512 || meta.formatId != CLIPP_FORMAT_UTF8) {
			meta.isCompressed = 0;
			meta.payloadDataSize = static_cast<uint64_t>(rawData.size());
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
		meta.isCompressed = 1;
		meta.payloadDataSize = static_cast<uint64_t>(rawData.size());
		return true;
	}

	// Decompresses rawData if meta.isCompressed; verifies sizes against meta.
	// Updates meta.isCompressed, meta.payloadDataSize on success.
	bool ZstdDecompress() {
		const uint64_t expectedUncompressedSize = meta.uncompressedDataSize;
		if (expectedUncompressedSize > ClipboardLimits::kMaxDecompressedClipboardBytes) {
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard payload: uncompressed size %llu bytes exceeds limit %llu bytes",
				static_cast<unsigned long long>(expectedUncompressedSize),
				ClipboardLimits::kMaxDecompressedClipboardBytes);
			return false;
		}

		if (meta.isCompressed == 0) {
			if (rawData.size() != expectedUncompressedSize) {
				g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting uncompressed clipboard payload: size mismatch (expected %llu bytes, actual %zu bytes)",
					static_cast<unsigned long long>(expectedUncompressedSize), rawData.size());
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
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: frame uncompressed size mismatch (expected %llu bytes, frame reports %llu bytes)",
				static_cast<unsigned long long>(expectedUncompressedSize), decompressedSize);
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
			g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting compressed clipboard payload: decompressed size mismatch (expected %llu bytes, actual %zu bytes)",
				static_cast<unsigned long long>(expectedUncompressedSize), actualSize);
			return false;
		}

		g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Decompressed data from %zu bytes to %zu bytes", rawData.size(), actualSize);
		rawData = std::move(decompressedData);
		meta.isCompressed = 0;
		meta.payloadDataSize = static_cast<uint64_t>(rawData.size());
		return true;
	}
};
