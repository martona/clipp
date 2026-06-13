#pragma once

#include "Hlc.h"
#include "HostId.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Per-record flag bits. Fixed now (record AND wire) so the format never has to
// change retroactively. TOMBSTONE and PRIVATE are the only *record* flags;
// transport flags (touch-only, relay) ride the REGW frame, not the record.
namespace RegisterFlags {
inline constexpr uint8_t Tombstone = 0x01;  // delete marker; value is empty
inline constexpr uint8_t Private   = 0x02;  // mask in `ls -v`; isatty-gate on `paste`
// 0x04 .. 0x80 reserved.
}

// One entry in the LWW-element-map. A value record and a tombstone share this
// shape; the TOMBSTONE flag distinguishes them. `written` and `touched` are
// independent HLCs (see RegisterStore).
struct RegisterRecord {
    std::string name;       // "" is reserved for the default-clipboard mirror; never user-addressable
    std::string value;      // raw bytes (UTF-8 text in v1); empty for a tombstone
    Hlc written;            // advances on writes only; sole input to value conflict resolution
    Hlc touched;            // advances on reads and writes; sole input to expiry
    HostId originHostId;    // LWW tiebreak when `written` ties
    uint8_t flags{ 0 };

    bool IsTombstone() const { return (flags & RegisterFlags::Tombstone) != 0; }
    bool IsPrivate() const { return (flags & RegisterFlags::Private) != 0; }

    // Full-field equality; used to detect "merge changed nothing" and by tests to
    // assert replica convergence.
    bool operator==(const RegisterRecord&) const = default;
};

// Compact metadata for anti-entropy (RSYN) and `ls`. Carries no value bytes.
struct RegisterDigestEntry {
    std::string name;
    Hlc written;
    Hlc touched;
};

// ---- Pure helpers (no locking, no clock): the merge core, independently tested ----

// Register names are case-sensitive ASCII [a-z0-9._-]{1,64}. The empty name is NOT
// valid here (it is the internal mirror key). Enforce at the CLI and at REGW
// ingress (defense in depth).
bool IsValidRegisterName(std::string_view name);

// True iff `r` has out-lived its idle TTL in real wall time. Pure function of the
// record and the supplied now — the same inputs yield the same answer on every
// replica. A `touched` in the future (a fast peer's clock) is treated as not-yet-
// expired rather than overflowing.
bool IsExpired(const RegisterRecord& r, uint64_t nowMs, uint64_t ttlMs);

// The LWW join of two records for the SAME name. Value, flags, origin and
// `written` come from the winner of the total order (written, originHostId, flags,
// value); `touched` is the independent max of both sides. Commutative,
// associative, idempotent — the convergence (SEC) guarantee rests on this. The
// flags/value tail of the order is only a robustness tiebreak; under the normal
// invariant (a given (originHostId, written) maps to one write) it never decides.
RegisterRecord MergeRecords(const RegisterRecord& a, const RegisterRecord& b);

// In-RAM, convergent named-register store. Pure merge logic lives in the free
// functions above; this class is the thread-safe container around them, mirroring
// ClipboardActivityStore's self-synchronized design. Expiry is the GC: every
// surface that walks records erases expired ones on the way (drop-on-access), so
// the physical map equals the live set and no sweep timer is needed.
class RegisterStore {
public:
    static constexpr uint64_t kDefaultTtlMs = 90ull * 24 * 60 * 60 * 1000;  // 90 days
    static constexpr size_t kDefaultMaxCount = 1024;
    static constexpr size_t kDefaultMaxValueBytes = 64ull * 1024 * 1024;    // matches the wire frame cap

    enum class WriteResult { Ok, InvalidName, ValueTooLarge, CapExceeded };
    enum class DeleteResult { Deleted, NotFound };

    // Default-constructed for the daemon global; configure with SetLocalHost +
    // SetLimits at startup (mirrors ClipboardActivityStore's default-construct +
    // SetLimits pattern). Uses the real wall clock.
    RegisterStore();

    explicit RegisterStore(HostId localHost,
                           uint64_t ttlMs = kDefaultTtlMs,
                           size_t maxCount = kDefaultMaxCount,
                           size_t maxValueBytes = kDefaultMaxValueBytes,
                           std::function<uint64_t()> nowMs = &HlcClock::SystemNowMs);

    // --- Local user operations (originate replicated state) ---

    // Insert or overwrite `name` with `value`. Refused for an invalid name, an
    // over-cap value, or when this would create a NEW live register past the count
    // cap (overwriting an existing live one is always allowed). `isPrivate` sets
    // the PRIVATE flag.
    WriteResult Upsert(const std::string& name, std::string value, bool isPrivate = false);

    // `paste`: returns the record and refreshes `touched` (LRU). nullopt if absent,
    // tombstoned, or expired.
    std::optional<RegisterRecord> Read(const std::string& name);

    // Tombstone a live register. Returns NotFound (writing nothing) when there is
    // no live value to delete — the CLI turns that into "no such register".
    DeleteResult Delete(const std::string& name);

    // Observational default-register mirror: reflect the live OS clipboard under
    // the reserved "" key so `ls` can show it. Local-only — never replicated (the
    // existing CLIP sync already moves the unnamed clipboard) and never counted
    // against the register cap. Text-only in v1.
    void MirrorDefault(std::string value);

    // --- Inspection (does NOT refresh `touched`) ---

    std::vector<std::string> ListNames();                 // live names, sorted (`ls`)
    std::vector<RegisterRecord> List();                   // live records, sorted (`ls -v`)

    // --- Replication ---

    // Digest of live records for anti-entropy, name-sorted. Expired records are
    // pruned at build time, so they are never advertised.
    std::vector<RegisterDigestEntry> Digest();

    // All non-expired records — values AND tombstones — name-sorted. The full set
    // a peer pulls during anti-entropy (tombstones must replicate to keep deletes
    // sticky). Prunes expired on the way out.
    std::vector<RegisterRecord> SnapshotForSync();

    // Names this store should push (as REGW) given a peer's digest: records the
    // peer lacks, or for which it holds a staler (written, touched). Pure planning
    // step for the Phase 3 wire protocol; exercised in-process by the tests.
    std::vector<std::string> PlanPush(const std::vector<RegisterDigestEntry>& remoteDigest);

    // Merge a record received from a peer. Witnesses both HLCs, ignores a record
    // already expired by local clock, and applies the LWW join. Deliberately
    // BYPASSES the count cap — the cap gates local origination only; refusing a
    // replicated record would diverge replicas. Returns true if local state
    // changed.
    bool ApplyRemote(RegisterRecord incoming);

    // --- Lifecycle / introspection ---

    void SetLocalHost(const HostId& host);                          // origin for local writes (LWW tiebreak)
    void SetLimits(uint64_t ttlMs, size_t maxCount, size_t maxValueBytes);
    void SeedClockFloor(const Hlc& floor);   // startup: lift the clock to the persisted floor
    Hlc ClockHighWater() const;              // for floor persistence (Phase 2)
    size_t LiveCount() const;                // live values (excludes tombstones, expired)
    size_t PhysicalCount() const;            // all resident records (drop-on-access test seam)

private:
    void PruneExpiredLocked();
    size_t LiveCountLocked() const;

    mutable std::mutex mutex_;
    HlcClock clock_;
    std::map<std::string, RegisterRecord> records_;
    HostId localHost_;
    uint64_t ttlMs_;
    size_t maxCount_;
    size_t maxValueBytes_;
    std::function<uint64_t()> nowMs_;
};

// Daemon-global instance (defined in main.cpp for desktop, ClippBridge.mm for
// iOS). Absent from the headless Linux CLI build, which runs no register daemon.
extern RegisterStore g_registerStore;
