#include "RegisterStore.h"

#include "platform.h"  // clipp_platform_detail::DecodeUtf8CodePoint (name validation)

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace {

// Strict-weak total order over a record's value identity. The "greater" record
// wins the value. (written, originHostId) is the semantic order; flags and value
// are appended only so the join stays total — and therefore commutative — even for
// malformed or forged duplicates. Under the normal invariant (a given
// (originHostId, written) maps to exactly one write) the tail never decides.
bool ValueLess(const RegisterRecord& a, const RegisterRecord& b) {
    if (a.written != b.written) return a.written < b.written;
    if (!(a.originHostId == b.originHostId)) return a.originHostId < b.originHostId;
    if (a.flags != b.flags) return a.flags < b.flags;
    return a.value < b.value;
}

}  // namespace

bool IsValidRegisterName(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;  // 64 BYTES: the wire cap old builds enforce at decode
    }
    if (name.front() == ' ' || name.back() == ' ') {
        return false;  // edge whitespace: invisible in UI, an addressing trap
    }
    std::size_t offset = 0;
    while (offset < name.size()) {
        uint32_t codePoint = 0;
        if (!clipp_platform_detail::DecodeUtf8CodePoint(name, offset, codePoint)) {
            return false;  // malformed UTF-8 (overlongs, surrogates, truncation)
        }
        if (codePoint < 0x20 || codePoint == 0x7F ||
            (codePoint >= 0x80 && codePoint <= 0x9F)) {
            return false;  // C0/C1/DEL: `ls` is a line-oriented script surface
        }
        if (codePoint == '?' || codePoint == '*' || codePoint == '/') {
            return false;  // reserved printables: globs stay pure CLI
                           // metacharacters; '/' is for future namespaces
        }
    }
    return true;
}

bool IsExpired(const RegisterRecord& r, uint64_t nowMs, uint64_t ttlMs) {
    // A touch in the future (a fast peer's wall clock) is not yet expired; this
    // branch also avoids the wraparound that touched + ttl could hit.
    if (r.touched.wallMs > nowMs) {
        return false;
    }
    return (nowMs - r.touched.wallMs) >= ttlMs;
}

RegisterRecord MergeRecords(const RegisterRecord& a, const RegisterRecord& b) {
    // Caller guarantees a.name == b.name.
    RegisterRecord out = ValueLess(a, b) ? b : a;       // value, flags, origin, written
    out.touched = std::max(a.touched, b.touched);        // independent lattice join
    return out;
}

RegisterStore::RegisterStore(HostId localHost, uint64_t ttlMs, size_t maxCount,
                             size_t maxValueBytes, std::function<uint64_t()> nowMs)
    : clock_(nowMs),                       // copy: clock_ is declared before nowMs_
      localHost_(localHost),
      ttlMs_(ttlMs),
      maxCount_(maxCount),
      maxValueBytes_(maxValueBytes),
      nowMs_(std::move(nowMs)) {}

RegisterStore::RegisterStore()
    : RegisterStore(HostId{}, kDefaultTtlMs, kDefaultMaxCount, kDefaultMaxValueBytes,
                    &HlcClock::SystemNowMs) {}

void RegisterStore::SetLocalHost(const HostId& host) {
    std::lock_guard<std::mutex> lock(mutex_);
    localHost_ = host;
}

void RegisterStore::SetLimits(uint64_t ttlMs, size_t maxCount, size_t maxValueBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    ttlMs_ = ttlMs;
    maxCount_ = maxCount;
    maxValueBytes_ = maxValueBytes;
}

void RegisterStore::PruneExpiredLocked() {
    const uint64_t now = nowMs_();
    for (auto it = records_.begin(); it != records_.end();) {
        if (IsExpired(it->second, now, ttlMs_)) {
            it = records_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t RegisterStore::LiveCountLocked() const {
    const uint64_t now = nowMs_();
    size_t n = 0;
    for (const auto& [name, r] : records_) {
        if (name.empty()) continue;  // the "" mirror never counts against the user cap
        if (!r.IsTombstone() && !IsExpired(r, now, ttlMs_)) {
            ++n;
        }
    }
    return n;
}

RegisterStore::WriteResult RegisterStore::UpsertLocked(const std::string& name, std::string value,
                                                      uint8_t valueFlags, const HostId& origin) {
    // "" is the reserved mirror key, written only via MirrorDefault — reject it
    // (and any invalid name) from the user-write path.
    if (!IsValidRegisterName(name)) {
        return WriteResult::InvalidName;
    }
    if (value.size() > maxValueBytes_) {
        return WriteResult::ValueTooLarge;
    }

    PruneExpiredLocked();  // reclaim dead slots so the cap counts live records exactly

    const auto it = records_.find(name);
    const bool currentlyLiveValue = (it != records_.end()) && !it->second.IsTombstone();
    if (!currentlyLiveValue && LiveCountLocked() >= maxCount_) {
        return WriteResult::CapExceeded;
    }

    // A locally generated HLC sits above everything witnessed, so a fresh write
    // dominates any prior record for this name — assign outright, no merge needed.
    const Hlc ts = clock_.Now();
    RegisterRecord r;
    r.name = name;
    r.value = std::move(value);
    r.written = ts;
    r.touched = ts;
    r.originHostId = origin;
    // Tombstone is a lifecycle state, never a caller-supplied value flag.
    r.flags = static_cast<uint8_t>(valueFlags & ~RegisterFlags::Tombstone);
    records_.insert_or_assign(name, std::move(r));
    return WriteResult::Ok;
}

RegisterStore::WriteResult RegisterStore::Upsert(const std::string& name, std::string value,
                                                 bool isPrivate) {
    std::lock_guard<std::mutex> lock(mutex_);
    return UpsertLocked(name, std::move(value),
                        isPrivate ? RegisterFlags::Private : uint8_t{ 0 }, localHost_);
}

RegisterStore::WriteResult RegisterStore::Upsert(const std::string& name, std::string value,
                                                 bool isPrivate, const HostId& origin) {
    std::lock_guard<std::mutex> lock(mutex_);
    return UpsertLocked(name, std::move(value),
                        isPrivate ? RegisterFlags::Private : uint8_t{ 0 }, origin);
}

RegisterStore::WriteResult RegisterStore::UpsertWithFlags(const std::string& name, std::string value,
                                                          uint8_t valueFlags, const HostId& origin) {
    std::lock_guard<std::mutex> lock(mutex_);
    return UpsertLocked(name, std::move(value), valueFlags, origin);
}

std::optional<RegisterRecord> RegisterStore::Read(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = records_.find(name);
    if (it == records_.end()) {
        return std::nullopt;
    }
    if (IsExpired(it->second, nowMs_(), ttlMs_)) {
        records_.erase(it);  // drop-on-access
        return std::nullopt;
    }
    if (it->second.IsTombstone()) {
        return std::nullopt;
    }
    it->second.touched = clock_.Now();  // LRU refresh
    return it->second;
}

RegisterStore::DeleteResult RegisterStore::DeleteLocked(const std::string& name, const HostId& origin) {
    const auto it = records_.find(name);
    if (it == records_.end()) {
        return DeleteResult::NotFound;
    }
    if (IsExpired(it->second, nowMs_(), ttlMs_)) {
        records_.erase(it);  // drop-on-access
        return DeleteResult::NotFound;
    }
    if (it->second.IsTombstone()) {
        return DeleteResult::NotFound;  // already deleted
    }

    // Tombstone the live value. The fresh HLC dominates the value's `written`
    // (clock has witnessed it), and its `touched` is >= the value's; the mesh's
    // max-merge of `touched` then keeps the tombstone alive at least as long as
    // anything it shadows.
    const Hlc ts = clock_.Now();
    RegisterRecord& r = it->second;
    r.value.clear();
    r.written = ts;
    r.touched = ts;
    r.originHostId = origin;
    r.flags = RegisterFlags::Tombstone;
    return DeleteResult::Deleted;
}

RegisterStore::DeleteResult RegisterStore::Delete(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DeleteLocked(name, localHost_);
}

RegisterStore::DeleteResult RegisterStore::Delete(const std::string& name, const HostId& origin) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DeleteLocked(name, origin);
}

void RegisterStore::MirrorDefault(std::string value) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Observational only: overwrite the "" record with the current clipboard text.
    // No cap, no validation; the replication and cap surfaces all skip "".
    const Hlc ts = clock_.Now();
    RegisterRecord r;
    r.value = std::move(value);
    r.written = ts;
    r.touched = ts;
    r.originHostId = localHost_;
    r.flags = 0;
    records_.insert_or_assign(std::string(), std::move(r));
}

std::vector<std::string> RegisterStore::ListNames() {
    std::lock_guard<std::mutex> lock(mutex_);
    PruneExpiredLocked();
    const uint64_t now = nowMs_();
    std::vector<std::string> names;
    for (const auto& [name, r] : records_) {  // std::map iterates name-sorted
        if (!r.IsTombstone() && !IsExpired(r, now, ttlMs_)) {
            names.push_back(name);
        }
    }
    return names;
}

std::vector<RegisterRecord> RegisterStore::List() {
    std::lock_guard<std::mutex> lock(mutex_);
    PruneExpiredLocked();
    const uint64_t now = nowMs_();
    std::vector<RegisterRecord> out;
    for (const auto& [name, r] : records_) {
        if (!r.IsTombstone() && !IsExpired(r, now, ttlMs_)) {
            out.push_back(r);
        }
    }
    return out;
}

std::vector<RegisterDigestEntry> RegisterStore::Digest() {
    std::lock_guard<std::mutex> lock(mutex_);
    PruneExpiredLocked();  // never advertise an expired record
    std::vector<RegisterDigestEntry> out;
    out.reserve(records_.size());
    for (const auto& [name, r] : records_) {
        if (name.empty()) continue;  // mirror is local-only, never advertised
        out.push_back(RegisterDigestEntry{ name, r.written, r.touched });
    }
    return out;
}

std::vector<RegisterRecord> RegisterStore::SnapshotForSync() {
    std::lock_guard<std::mutex> lock(mutex_);
    PruneExpiredLocked();
    std::vector<RegisterRecord> out;
    out.reserve(records_.size());
    for (const auto& [name, r] : records_) {
        if (name.empty()) continue;  // mirror is local-only, never replicated
        out.push_back(r);
    }
    return out;
}

std::optional<RegisterRecord> RegisterStore::GetForBroadcast(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = records_.find(name);
    if (it == records_.end()) {
        return std::nullopt;
    }
    if (IsExpired(it->second, nowMs_(), ttlMs_)) {
        records_.erase(it);  // drop-on-access
        return std::nullopt;
    }
    return it->second;  // value or tombstone, no touch
}

std::vector<RegisterRecord> RegisterStore::RecordsToPush(
    const std::vector<RegisterDigestEntry>& remoteDigest) {
    std::lock_guard<std::mutex> lock(mutex_);
    PruneExpiredLocked();

    std::unordered_map<std::string_view, const RegisterDigestEntry*> remote;
    remote.reserve(remoteDigest.size());
    for (const auto& e : remoteDigest) {
        remote.emplace(e.name, &e);
    }

    std::vector<RegisterRecord> out;
    for (const auto& [name, r] : records_) {
        if (name.empty()) continue;  // mirror is local-only, never pushed
        const auto found = remote.find(name);
        if (found == remote.end()) {
            out.push_back(r);  // peer lacks it entirely (value or tombstone)
            continue;
        }
        const RegisterDigestEntry& their = *found->second;
        if (r.written > their.written ||
            (r.written == their.written && r.touched > their.touched)) {
            out.push_back(r);  // we hold a newer value, or the same value with a fresher touch
        }
    }
    return out;
}

std::vector<std::string> RegisterStore::PlanPush(
    const std::vector<RegisterDigestEntry>& remoteDigest) {
    // Names-only view; delegates (no lock here — RecordsToPush takes it).
    std::vector<std::string> names;
    for (const auto& r : RecordsToPush(remoteDigest)) {
        names.push_back(r.name);
    }
    return names;
}

bool RegisterStore::ApplyRemote(RegisterRecord incoming) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (incoming.name.empty()) {
        return false;  // "" is the local-only mirror; a peer can never inject it
    }
    clock_.Witness(incoming.written);
    clock_.Witness(incoming.touched);

    const uint64_t now = nowMs_();
    if (IsExpired(incoming, now, ttlMs_)) {
        return false;  // never accept state that is already dead by our clock
    }

    const auto it = records_.find(incoming.name);
    if (it == records_.end()) {
        // Copy the key out BEFORE moving `incoming` — they alias the same object
        // and argument evaluation order is unspecified (MSVC evaluates arguments
        // right-to-left, which would move `incoming` before reading its name and
        // store the record under an empty key). Cap is bypassed by design.
        std::string name = incoming.name;
        records_.insert_or_assign(std::move(name), std::move(incoming));
        return true;
    }
    if (IsExpired(it->second, now, ttlMs_)) {
        it->second = std::move(incoming);  // our copy is dead; incoming supersedes it
        return true;
    }

    RegisterRecord merged = MergeRecords(it->second, incoming);
    if (merged == it->second) {
        return false;  // incoming was dominated and carried no fresher touch
    }
    it->second = std::move(merged);
    return true;
}

void RegisterStore::SeedClockFloor(const Hlc& floor) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_.SeedFloor(floor);
}

Hlc RegisterStore::ClockHighWater() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clock_.HighWater();
}

size_t RegisterStore::LiveCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return LiveCountLocked();
}

size_t RegisterStore::PhysicalCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}
