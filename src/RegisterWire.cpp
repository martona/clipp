#include "RegisterWire.h"

#include <array>
#include <cstring>

namespace RegisterWire {

namespace {

constexpr uint8_t kVersion = 1;

void PutU16(std::vector<unsigned char>& b, uint16_t v) {
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    b.push_back(static_cast<unsigned char>(v & 0xFF));
}

void PutU32(std::vector<unsigned char>& b, uint32_t v) {
    b.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    b.push_back(static_cast<unsigned char>(v & 0xFF));
}

void PutBytes(std::vector<unsigned char>& b, const void* p, size_t n) {
    const auto* bytes = static_cast<const unsigned char*>(p);
    b.insert(b.end(), bytes, bytes + n);
}

void PutHlc(std::vector<unsigned char>& b, const Hlc& h) {
    const std::array<uint8_t, 16> packed = h.Pack();
    b.insert(b.end(), packed.begin(), packed.end());
}

// Cursor over a frame body. Every read is bounds-checked; a false return means the
// frame is truncated/malformed and the caller bails.
struct Reader {
    const unsigned char* p;
    size_t remaining;

    bool U8(uint8_t& out) {
        if (remaining < 1) return false;
        out = *p++;
        --remaining;
        return true;
    }
    bool U16(uint16_t& out) {
        if (remaining < 2) return false;
        out = static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
        p += 2;
        remaining -= 2;
        return true;
    }
    bool U32(uint32_t& out) {
        if (remaining < 4) return false;
        out = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
              (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
        p += 4;
        remaining -= 4;
        return true;
    }
    bool ReadHlc(Hlc& out) {
        if (remaining < 16) return false;
        std::array<uint8_t, 16> a{};
        std::memcpy(a.data(), p, 16);
        out = Hlc::Unpack(a);
        p += 16;
        remaining -= 16;
        return true;
    }
    bool ReadHostId(HostId& out) {
        if (remaining < HostId::kSize) return false;
        out = HostId(p);
        p += HostId::kSize;
        remaining -= HostId::kSize;
        return true;
    }
    bool ReadString(std::string& out, size_t n) {
        if (remaining < n) return false;
        out.assign(reinterpret_cast<const char*>(p), n);
        p += n;
        remaining -= n;
        return true;
    }
};

}  // namespace

std::vector<unsigned char> EncodeRecord(const RegisterRecord& record, uint8_t transportFlags) {
    std::vector<unsigned char> b;
    b.reserve(56 + record.name.size() + record.value.size());
    b.push_back(kVersion);
    b.push_back(record.flags);          // record flags (TOMBSTONE | PRIVATE)
    b.push_back(transportFlags);
    b.push_back(0);                     // reserved
    PutHlc(b, record.written);
    PutHlc(b, record.touched);
    PutBytes(b, record.originHostId.data().data(), HostId::kSize);
    PutU16(b, static_cast<uint16_t>(record.name.size()));
    PutBytes(b, record.name.data(), record.name.size());
    PutU32(b, static_cast<uint32_t>(record.value.size()));
    PutBytes(b, record.value.data(), record.value.size());
    return b;
}

bool TryDecodeRecord(const std::vector<unsigned char>& body, RegisterRecord& outRecord,
                     uint8_t& outTransportFlags) {
    Reader r{ body.data(), body.size() };
    uint8_t version = 0;
    uint8_t recordFlags = 0;
    uint8_t transport = 0;
    uint8_t reserved = 0;
    if (!r.U8(version) || version != kVersion) return false;
    if (!r.U8(recordFlags) || !r.U8(transport) || !r.U8(reserved)) return false;

    Hlc written;
    Hlc touched;
    HostId origin;
    if (!r.ReadHlc(written) || !r.ReadHlc(touched) || !r.ReadHostId(origin)) return false;

    uint16_t nameLen = 0;
    if (!r.U16(nameLen) || nameLen > kMaxNameLen) return false;
    std::string name;
    if (!r.ReadString(name, nameLen)) return false;

    uint32_t valueLen = 0;
    if (!r.U32(valueLen) || valueLen > kMaxValueLen) return false;
    std::string value;
    if (!r.ReadString(value, valueLen)) return false;

    if (r.remaining != 0) return false;  // trailing garbage -> reject

    outRecord.name = std::move(name);
    outRecord.value = std::move(value);
    outRecord.written = written;
    outRecord.touched = touched;
    outRecord.originHostId = origin;
    outRecord.flags = recordFlags;
    outTransportFlags = transport;
    return true;
}

std::vector<unsigned char> EncodeDigest(const std::vector<RegisterDigestEntry>& entries) {
    std::vector<unsigned char> b;
    const uint16_t count =
        static_cast<uint16_t>(entries.size() > kMaxDigestEntries ? kMaxDigestEntries : entries.size());
    b.reserve(3 + static_cast<size_t>(count) * 40);
    b.push_back(kVersion);
    PutU16(b, count);
    for (uint16_t i = 0; i < count; ++i) {
        const RegisterDigestEntry& e = entries[i];
        PutU16(b, static_cast<uint16_t>(e.name.size()));
        PutBytes(b, e.name.data(), e.name.size());
        PutHlc(b, e.written);
        PutHlc(b, e.touched);
    }
    return b;
}

bool TryDecodeDigest(const std::vector<unsigned char>& body,
                     std::vector<RegisterDigestEntry>& outEntries) {
    Reader r{ body.data(), body.size() };
    uint8_t version = 0;
    if (!r.U8(version) || version != kVersion) return false;
    uint16_t count = 0;
    if (!r.U16(count) || count > kMaxDigestEntries) return false;

    outEntries.clear();
    outEntries.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t nameLen = 0;
        if (!r.U16(nameLen) || nameLen > kMaxNameLen) return false;
        RegisterDigestEntry e;
        if (!r.ReadString(e.name, nameLen)) return false;
        if (!r.ReadHlc(e.written) || !r.ReadHlc(e.touched)) return false;
        outEntries.push_back(std::move(e));
    }
    if (r.remaining != 0) return false;
    return true;
}

std::vector<unsigned char> EncodeName(const std::string& name) {
    std::vector<unsigned char> b;
    b.reserve(3 + name.size());
    b.push_back(kVersion);
    PutU16(b, static_cast<uint16_t>(name.size()));
    PutBytes(b, name.data(), name.size());
    return b;
}

bool TryDecodeName(const std::vector<unsigned char>& body, std::string& outName) {
    Reader r{ body.data(), body.size() };
    uint8_t version = 0;
    if (!r.U8(version) || version != kVersion) return false;
    uint16_t nameLen = 0;
    if (!r.U16(nameLen) || nameLen > kMaxNameLen) return false;
    if (!r.ReadString(outName, nameLen)) return false;
    if (r.remaining != 0) return false;
    return true;
}

}  // namespace RegisterWire
