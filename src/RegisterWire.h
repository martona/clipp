#pragma once

#include "RegisterStore.h"  // RegisterRecord, RegisterDigestEntry

#include <cstdint>
#include <string>
#include <vector>

// Wire (de)serialization for the named-register frames. Pure byte packing — no
// sockets, no crypto: the caller hands the returned body to CryptoChannel::Send-
// Frame(tag, body), and feeds the post-tag body slice of a received frame to the
// matching decoder. Every multi-byte field is big-endian and fixed-width so the
// bytes decode identically on every platform. Decoders are defensive — they
// bounds-check every length, reject trailing garbage, and reject unknown versions
// (forward-compatible: an older peer log-and-ignores a newer frame).
namespace RegisterWire {

// Transport flags carried on a REGW frame. NOT persisted into the record — they
// describe how to handle the frame, not the data.
namespace Transport {
inline constexpr uint8_t Relay = 0x01;      // gateway should rebroadcast to the mesh
inline constexpr uint8_t TouchOnly = 0x02;  // reserved; v1 always sends the full record
}

// Hard caps the decoders enforce on inbound frames (defense in depth; the engine
// enforces its own name/value/count limits too).
inline constexpr uint16_t kMaxNameLen = 64;                       // matches IsValidRegisterName
inline constexpr uint32_t kMaxValueLen = 64u * 1024u * 1024u;     // matches the frame payload cap
inline constexpr uint16_t kMaxDigestEntries = 1024;               // matches RegisterMaxCount
inline constexpr uint16_t kMaxPreviewLen = 256;                  // value bytes carried per list entry

// One entry in an RLST list response — richer than a digest: metadata plus a
// capped, possibly-empty value preview, for `ls -v`. (The anti-entropy RSYN digest
// stays name/written/touched only; this is the CLI-facing list.)
struct RegisterListEntry {
    std::string name;        // "" = the default-clipboard mirror
    Hlc touched;             // for age
    uint64_t valueSize{ 0 }; // full value size in bytes
    HostId originHostId;
    uint8_t flags{ 0 };      // PRIVATE etc.
    std::string preview;     // up to kMaxPreviewLen bytes; empty for private/empty values
};

// REGW: a full register record (value or tombstone) plus transport flags.
std::vector<unsigned char> EncodeRecord(const RegisterRecord& record, uint8_t transportFlags);
bool TryDecodeRecord(const std::vector<unsigned char>& body, RegisterRecord& outRecord,
                     uint8_t& outTransportFlags);

// RSYN: the (name, written, touched) digest of every live register.
std::vector<unsigned char> EncodeDigest(const std::vector<RegisterDigestEntry>& entries);
bool TryDecodeDigest(const std::vector<unsigned char>& body,
                     std::vector<RegisterDigestEntry>& outEntries);

// RGET: a single register name to fetch.
std::vector<unsigned char> EncodeName(const std::string& name);
bool TryDecodeName(const std::vector<unsigned char>& body, std::string& outName);

// RLST response: the rich list (name + metadata + capped preview) for `ls`/`ls -v`.
std::vector<unsigned char> EncodeList(const std::vector<RegisterListEntry>& entries);
bool TryDecodeList(const std::vector<unsigned char>& body, std::vector<RegisterListEntry>& outEntries);

}  // namespace RegisterWire
