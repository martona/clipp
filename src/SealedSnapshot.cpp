#include "SealedSnapshot.h"

#include <sodium.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace SealedSnapshot {

namespace {

constexpr size_t kMagicSize = 4;
constexpr size_t kHeaderSize = kMagicSize + 1;  // magic + version = the AEAD's AD
constexpr size_t kNonceSize = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr size_t kTagSize = crypto_aead_xchacha20poly1305_ietf_ABYTES;

static_assert(kSealKeyBytes == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);

void AppendU32(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back(static_cast<unsigned char>(v >> 24));
    out.push_back(static_cast<unsigned char>(v >> 16));
    out.push_back(static_cast<unsigned char>(v >> 8));
    out.push_back(static_cast<unsigned char>(v));
}

bool ReadU32(const std::vector<unsigned char>& in, size_t& offset, uint32_t& v) {
    if (in.size() - offset < 4) {
        return false;
    }
    v = (static_cast<uint32_t>(in[offset]) << 24)
      | (static_cast<uint32_t>(in[offset + 1]) << 16)
      | (static_cast<uint32_t>(in[offset + 2]) << 8)
      | static_cast<uint32_t>(in[offset + 3]);
    offset += 4;
    return true;
}

std::filesystem::path PathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// fopen with a wide path on Windows (the narrow CRT open would go through the
// ANSI code page and mangle non-ASCII profile names).
std::FILE* OpenFile(const std::filesystem::path& path, bool forWrite) {
#ifdef _WIN32
    return _wfopen(path.c_str(), forWrite ? L"wb" : L"rb");
#else
    return std::fopen(path.c_str(), forWrite ? "wb" : "rb");
#endif
}

}  // namespace

std::vector<unsigned char> Seal(const Magic& magic, uint8_t version,
                                const std::vector<std::vector<unsigned char>>& records,
                                const SealKey& key) {
    std::vector<unsigned char> plain;
    // Count is patched in after the skip-on-empty loop.
    AppendU32(plain, 0);
    uint32_t sealedCount = 0;
    for (const auto& record : records) {
        if (record.empty()) {
            continue;  // encode-failure convention — skip, never abort
        }
        AppendU32(plain, static_cast<uint32_t>(record.size()));
        plain.insert(plain.end(), record.begin(), record.end());
        ++sealedCount;
    }
    plain[0] = static_cast<unsigned char>(sealedCount >> 24);
    plain[1] = static_cast<unsigned char>(sealedCount >> 16);
    plain[2] = static_cast<unsigned char>(sealedCount >> 8);
    plain[3] = static_cast<unsigned char>(sealedCount);

    std::vector<unsigned char> blob;
    blob.reserve(kHeaderSize + kNonceSize + plain.size() + kTagSize);
    blob.insert(blob.end(), magic.begin(), magic.end());
    blob.push_back(version);
    unsigned char nonce[kNonceSize];
    randombytes_buf(nonce, sizeof(nonce));
    blob.insert(blob.end(), nonce, nonce + kNonceSize);

    blob.resize(kHeaderSize + kNonceSize + plain.size() + kTagSize);
    unsigned long long cipherLen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        blob.data() + kHeaderSize + kNonceSize, &cipherLen,
        plain.data(), plain.size(),
        blob.data(), kHeaderSize,  // AD = magic + version (disjoint from the ct region)
        nullptr, nonce, key.data());
    sodium_memzero(plain.data(), plain.size());  // snapshot values may be sensitive
    blob.resize(kHeaderSize + kNonceSize + static_cast<size_t>(cipherLen));
    return blob;
}

bool TryOpen(const Magic& magic, uint8_t version,
             const std::vector<unsigned char>& blob, const SealKey& key,
             std::vector<std::vector<unsigned char>>& outRecords) {
    if (blob.size() < kHeaderSize + kNonceSize + kTagSize) {
        return false;
    }
    if (std::memcmp(blob.data(), magic.data(), kMagicSize) != 0 || blob[4] != version) {
        return false;
    }

    const unsigned char* nonce = blob.data() + kHeaderSize;
    const unsigned char* cipher = blob.data() + kHeaderSize + kNonceSize;
    const size_t cipherLen = blob.size() - kHeaderSize - kNonceSize;

    std::vector<unsigned char> plain(cipherLen - kTagSize);
    unsigned long long plainLen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            plain.data(), &plainLen, nullptr,
            cipher, cipherLen,
            blob.data(), kHeaderSize,
            nonce, key.data()) != 0) {
        return false;  // wrong key or tampering
    }
    plain.resize(static_cast<size_t>(plainLen));

    std::vector<std::vector<unsigned char>> records;
    size_t offset = 0;
    uint32_t count = 0;
    bool wellFormed = ReadU32(plain, offset, count) && count <= kMaxRecords;
    if (wellFormed) {
        records.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t len = 0;
            if (!ReadU32(plain, offset, len) || plain.size() - offset < len) {
                wellFormed = false;
                break;
            }
            records.emplace_back(plain.begin() + offset, plain.begin() + offset + len);
            offset += len;
        }
    }
    if (wellFormed && offset != plain.size()) {
        wellFormed = false;  // trailing garbage: the frame is lying about itself
    }
    sodium_memzero(plain.data(), plain.size());
    if (!wellFormed) {
        return false;
    }
    outRecords = std::move(records);
    return true;
}

bool SaveFile(const std::string& utf8Path, const Magic& magic, uint8_t version,
              const std::vector<std::vector<unsigned char>>& records,
              const SealKey& key) {
    const std::vector<unsigned char> blob = Seal(magic, version, records, key);
    if (blob.empty()) {
        return false;
    }

    const std::filesystem::path path = PathFromUtf8(utf8Path);
    std::filesystem::path temp = path;
    temp += ".tmp";

    std::FILE* file = OpenFile(temp, /*forWrite=*/true);
    if (file == nullptr) {
        return false;
    }
    const bool written =
        std::fwrite(blob.data(), 1, blob.size(), file) == blob.size()
        && std::fflush(file) == 0
#ifdef _WIN32
        && _commit(_fileno(file)) == 0;
#else
        && fsync(fileno(file)) == 0;
#endif
    std::fclose(file);
    if (!written) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);  // atomic replace on both platforms
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }
    return true;
}

LoadResult LoadFile(const std::string& utf8Path, const Magic& magic, uint8_t version,
                    const SealKey& key,
                    std::vector<std::vector<unsigned char>>& outRecords) {
    const std::filesystem::path path = PathFromUtf8(utf8Path);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return LoadResult::NoFile;
    }

    const auto quarantine = [&path]() {
        std::filesystem::path carcass = path;
        carcass += ".corrupt";
        std::error_code ignored;
        std::filesystem::remove(carcass, ignored);        // replace an older carcass
        std::filesystem::rename(path, carcass, ignored);  // best effort
    };

    std::vector<unsigned char> blob;
    {
        std::FILE* file = OpenFile(path, /*forWrite=*/false);
        if (file == nullptr) {
            quarantine();
            return LoadResult::Corrupt;
        }
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        bool readOk = size >= 0;
        if (readOk) {
            blob.resize(static_cast<size_t>(size));
            readOk = blob.empty()
                || std::fread(blob.data(), 1, blob.size(), file) == blob.size();
        }
        std::fclose(file);
        if (!readOk) {
            quarantine();
            return LoadResult::Corrupt;
        }
    }

    if (!TryOpen(magic, version, blob, key, outRecords)) {
        quarantine();
        return LoadResult::Corrupt;
    }
    return LoadResult::Loaded;
}

}  // namespace SealedSnapshot
