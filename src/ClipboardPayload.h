#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <sodium.h>
#include <xxhash.h>
#include <zstd.h>

#include "ClipboardFormat.h"
#include "ClipboardLimits.h"
#include "HostId.h"
#include "Logger.h"
#include "NetworkDefs.h"

// Canonicalize text line endings to LF in place: CRLF -> LF, and a lone CR -> LF.
// Only rebuilds the buffer when a CR is actually present. Shared by clipboard text
// (SetUncompressedBytes) and named-register copy (Cli.cpp) so the same text -- captured
// on any platform or piped through any shell -- settles to one canonical form.
inline void CanonicalizeCrlfToLf(std::vector<unsigned char>& bytes) {
    if (std::find(bytes.begin(), bytes.end(), static_cast<unsigned char>('\r')) == bytes.end()) {
        return;
    }
    std::vector<unsigned char> lf;
    lf.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r') {
            lf.push_back('\n');
            if (i + 1 < bytes.size() && bytes[i + 1] == '\n') {
                ++i;  // collapse CRLF to a single LF (lone CR also folds to LF)
            }
        } else {
            lf.push_back(bytes[i]);
        }
    }
    bytes = std::move(lf);
}

// Expand canonical-LF text to CRLF for egress where the platform's native line ending is
// CRLF (Windows): LF -> CRLF, a lone CR -> CRLF, and existing CRLF preserved (never
// CR-CR-LF). Only rebuilds when a line ending is present. The reverse of
// CanonicalizeCrlfToLf; callers apply it only where the native ending is CRLF.
inline void ExpandLfToCrlf(std::vector<unsigned char>& bytes) {
    if (std::find_if(bytes.begin(), bytes.end(),
                     [](unsigned char c) { return c == '\r' || c == '\n'; }) == bytes.end()) {
        return;
    }
    std::vector<unsigned char> out;
    out.reserve(bytes.size() + bytes.size() / 8 + 1);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char c = bytes[i];
        if (c == '\r' || c == '\n') {
            out.push_back('\r');
            out.push_back('\n');
            if (c == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') {
                ++i;  // consume the LF of a CRLF pair
            }
        } else {
            out.push_back(c);
        }
    }
    bytes = std::move(out);
}

// One in-memory representation of a clipboard item, end to end. `meta` is the
// same struct that travels on the wire (NetworkDefs::ClipboardMessage), so
// format / compression / sizes / hash / origin all live in one place.
//
// Bytes are private. Callers go through SetUncompressedBytes (when they have
// plaintext to send/store) or SetEncodedBytes (when they've parsed bytes off
// the wire). On the read side, EncodedBytes() returns the bytes as stored,
// and TryGetUncompressedBytes() returns plaintext — either the same storage
// (no copy, when the payload wasn't compressed in the first place) or a
// lazily-decompressed scratch buffer.
//
// Move-only. Typical lifecycle: build, wrap in shared_ptr<const>, share.
class ClipboardPayload {
public:
    NetworkDefs::ClipboardMessage meta{};

    ClipboardPayload() = default;
    ClipboardPayload(const ClipboardPayload&) = delete;
    ClipboardPayload& operator=(const ClipboardPayload&) = delete;
    ClipboardPayload(ClipboardPayload&& other) noexcept {
        meta = other.meta;
        encoded_ = std::move(other.encoded_);
        // Mutex / scratch / flags are NOT moved; this is a fresh instance.
        // The decompressed cache will be re-populated lazily on first read.
    }
    ClipboardPayload& operator=(ClipboardPayload&& other) noexcept {
        if (this == &other) return *this;
        meta = other.meta;
        encoded_ = std::move(other.encoded_);
        std::lock_guard<std::mutex> lock(scratchMutex_);
        decompressedScratch_.clear();
        decompressedScratch_.shrink_to_fit();
        scratchAttempted_ = false;
        scratchOk_ = false;
        localizedScratch_.clear();
        localizedScratch_.shrink_to_fit();
        localizedAttempted_ = false;
        localizedOk_ = false;
        return *this;
    }

    // Outgoing entry point: hand over plaintext bytes. Computes the hash over
    // the plaintext, compresses if profitable (text ≥ 512 bytes), and fills
    // meta.{hashAlg, hashBytes, isCompressed, uncompressedDataSize,
    // payloadDataSize}. meta.formatId must be set before this call. Returns
    // false on compression failure.
    bool SetUncompressedBytes(std::vector<unsigned char> bytes) {
        // Canonicalize line endings to LF for text before hashing/storing, so the wire
        // form is platform-neutral: the same text copied on Windows (CRLF) and macOS
        // (LF) hashes identically (so cross-platform dedup / the hash-guard see one
        // item), and receivers re-add the native ending on write (TryGetLocalizedBytes).
        if (meta.formatId == CLIPP_FORMAT_UTF8) {
            CanonicalizeCrlfToLf(bytes);
        }

        // Hash over plaintext (content identity, stable across compression).
        const XXH128_hash_t hash = XXH3_128bits_withSeed(
            bytes.data(),
            bytes.size(),
            static_cast<XXH64_hash_t>(meta.formatId));
        XXH128_canonical_t canonical{};
        XXH128_canonicalFromHash(&canonical, hash);
        static_assert(sizeof(canonical.digest) == 16, "XXH128 canonical digest must be 16 bytes");
        meta.hashAlg = 1;
        std::memset(meta.hashBytes, 0, sizeof(meta.hashBytes));
        std::memcpy(meta.hashBytes, canonical.digest, sizeof(canonical.digest));

        meta.uncompressedDataSize = static_cast<uint64_t>(bytes.size());

        const bool worthCompressing =
            bytes.size() >= 512 && meta.formatId == CLIPP_FORMAT_UTF8;

        if (!worthCompressing) {
            encoded_ = std::move(bytes);
            meta.isCompressed = 0;
            meta.payloadDataSize = static_cast<uint64_t>(encoded_.size());
            ResetScratch();
            return true;
        }

        const size_t bound = ZSTD_compressBound(bytes.size());
        std::vector<unsigned char> compressed(bound);
        const size_t compressedSize = ZSTD_compress(
            compressed.data(),
            compressed.size(),
            bytes.data(),
            bytes.size(),
            ZSTD_fast);
        if (ZSTD_isError(compressedSize) != 0) {
            return false;
        }

        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            L"Compressed data from %zu bytes to %zu bytes",
            bytes.size(), compressedSize);

        compressed.resize(compressedSize);
        encoded_ = std::move(compressed);
        meta.isCompressed = 1;
        meta.payloadDataSize = static_cast<uint64_t>(encoded_.size());
        ResetScratch();
        return true;
    }

    // Wire receive entry point: caller has parsed meta from the wire and now
    // hands over the body bytes exactly as received. meta describes what's in
    // `bytes`; no recompression, no hash recompute. Caller is responsible for
    // having validated meta.payloadDataSize against bytes.size().
    void SetEncodedBytes(std::vector<unsigned char> bytes) {
        encoded_ = std::move(bytes);
        ResetScratch();
    }

    // Stamps origin-side metadata onto meta: originHostId, originHostName
    // (UTF-8, NUL-padded/truncated to HOSTNAME_MAX_BYTES), a fresh random
    // eventGuid, wall-clock timestamp (ms since Unix epoch), and the caller-
    // provided originSequenceNumber. Call this on locally-originated payloads
    // BEFORE wrapping in shared_ptr<const> and broadcasting. Receivers do NOT
    // call this — they inherit the origin's stamps from the wire.
    //
    // The eventGuid is what makes activity-stream dedup and "give me everything
    // since X" sync queries possible — identical content copied twice gets
    // distinct GUIDs, where the hash alone would collide.
    void StampOrigin(const HostId& originHostId, const char* originHostNameUtf8, uint64_t originSequenceNumber) {
        std::memcpy(meta.originHostId, originHostId.data().data(), sizeof(meta.originHostId));
        strncpys(meta.originHostName, originHostNameUtf8 != nullptr ? originHostNameUtf8 : "");
        randombytes_buf(meta.eventGuid, sizeof(meta.eventGuid));
        meta.originSequenceNumber = originSequenceNumber;
        const auto now = std::chrono::system_clock::now();
        meta.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    }

    // Bytes as stored. For wire send and any "I want the storage form" caller.
    const std::vector<unsigned char>& EncodedBytes() const { return encoded_; }

    // Plaintext access. Fast path (meta.isCompressed == 0): returns &encoded_
    // with no copy and no lock. Slow path: lazy-fills an internal scratch
    // buffer on first call, returns a pointer to that. nullptr on
    // decompression failure or malformed data.
    const std::vector<unsigned char>* TryGetUncompressedBytes() const {
        if (meta.isCompressed == 0) {
            return &encoded_;
        }

        std::lock_guard<std::mutex> lock(scratchMutex_);
        if (scratchAttempted_) {
            return scratchOk_ ? &decompressedScratch_ : nullptr;
        }
        scratchAttempted_ = true;

        const uint64_t expectedUncompressedSize = meta.uncompressedDataSize;
        if (expectedUncompressedSize > ClipboardLimits::kMaxDecompressedClipboardBytes) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning,
                L"Rejecting clipboard payload: uncompressed size %llu bytes exceeds limit %llu bytes",
                static_cast<unsigned long long>(expectedUncompressedSize),
                ClipboardLimits::kMaxDecompressedClipboardBytes);
            return nullptr;
        }

        const unsigned long long frameSize = ZSTD_getFrameContentSize(encoded_.data(), encoded_.size());
        if (frameSize == ZSTD_CONTENTSIZE_ERROR || frameSize == ZSTD_CONTENTSIZE_UNKNOWN) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning,
                L"Rejecting clipboard payload: invalid or unknown zstd frame (compressed size: %zu bytes)",
                encoded_.size());
            return nullptr;
        }

        if (frameSize != expectedUncompressedSize) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning,
                L"Rejecting clipboard payload: frame size %llu bytes does not match meta %llu bytes",
                frameSize,
                static_cast<unsigned long long>(expectedUncompressedSize));
            return nullptr;
        }

        decompressedScratch_.assign(static_cast<size_t>(expectedUncompressedSize), 0);
        const size_t actualSize = ZSTD_decompress(
            decompressedScratch_.data(),
            decompressedScratch_.size(),
            encoded_.data(),
            encoded_.size());
        if (ZSTD_isError(actualSize) != 0) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning,
                L"Rejecting clipboard payload: zstd decompression failed (%hs)",
                ZSTD_getErrorName(actualSize));
            decompressedScratch_.clear();
            decompressedScratch_.shrink_to_fit();
            return nullptr;
        }
        if (actualSize != expectedUncompressedSize) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning,
                L"Rejecting clipboard payload: decompressed size mismatch (expected %llu, got %zu)",
                static_cast<unsigned long long>(expectedUncompressedSize),
                actualSize);
            decompressedScratch_.clear();
            decompressedScratch_.shrink_to_fit();
            return nullptr;
        }

        g_logger.log(__FUNCTION__, Logger::Level::Debug,
            L"Decompressed data from %zu bytes to %zu bytes",
            encoded_.size(), actualSize);

        scratchOk_ = true;
        return &decompressedScratch_;
    }

    // Plaintext in the LOCAL platform's line-ending convention — for writing to the OS
    // clipboard or the CLI's stdout. The wire/canonical form is always LF (see
    // SetUncompressedBytes); on Windows this re-expands LF -> CRLF for text into a
    // lazily-filled scratch (kept separate from decompressedScratch_, which must stay
    // the canonical form the size check / preview / hash-guard rely on). Everywhere
    // else, and for non-text, it's identical to TryGetUncompressedBytes with no copy.
    // nullptr if the plaintext is unavailable.
    const std::vector<unsigned char>* TryGetLocalizedBytes() const {
        const std::vector<unsigned char>* canonical = TryGetUncompressedBytes();
#ifdef _WIN32
        if (canonical == nullptr || meta.formatId != CLIPP_FORMAT_UTF8) {
            return canonical;
        }
        std::lock_guard<std::mutex> lock(scratchMutex_);
        if (localizedAttempted_) {
            return localizedOk_ ? &localizedScratch_ : canonical;
        }
        localizedAttempted_ = true;
        // Emit CRLF for every line ending (LF / CRLF / lone CR uniformly), so a payload from
        // an older peer that still carries CRLF doesn't come out CR-CR-LF. The receive path
        // can't pre-normalize (it would break the payloadDataSize wire check), so the
        // robustness lives here -- via the same expansion named-register paste uses.
        localizedScratch_.assign(canonical->begin(), canonical->end());
        ExpandLfToCrlf(localizedScratch_);
        localizedOk_ = true;
        return &localizedScratch_;
#else
        return canonical;  // macOS / iOS / Linux are LF-native: native == canonical
#endif
    }

    bool Empty() const { return encoded_.empty(); }

private:
    void ResetScratch() {
        std::lock_guard<std::mutex> lock(scratchMutex_);
        decompressedScratch_.clear();
        decompressedScratch_.shrink_to_fit();
        scratchAttempted_ = false;
        scratchOk_ = false;
        localizedScratch_.clear();
        localizedScratch_.shrink_to_fit();
        localizedAttempted_ = false;
        localizedOk_ = false;
    }

    std::vector<unsigned char> encoded_;

    mutable std::mutex scratchMutex_;
    mutable std::vector<unsigned char> decompressedScratch_;
    mutable bool scratchAttempted_{false};
    mutable bool scratchOk_{false};
    // Lazily-filled native-line-ending form of the text, for OS clipboard / stdout
    // writes on platforms whose native ending differs from the LF wire form (Windows).
    // Separate from decompressedScratch_ so the canonical LF form survives.
    mutable std::vector<unsigned char> localizedScratch_;
    mutable bool localizedAttempted_{false};
    mutable bool localizedOk_{false};
};
