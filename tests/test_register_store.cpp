#include <doctest/doctest.h>

#include "Hlc.h"
#include "HostId.h"
#include "RegisterStore.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using WriteResult = RegisterStore::WriteResult;
using DeleteResult = RegisterStore::DeleteResult;

HostId MakeHost(uint8_t tag) {
    HostId::Bytes b{};
    b[0] = tag;
    return HostId(b);
}

// A realistic ms-since-epoch (~2023-11-14) so wall values look like the real thing.
constexpr uint64_t kEpoch = 1'700'000'000'000ull;

struct FakeTime {
    uint64_t ms;
};

std::function<uint64_t()> TimeFn(const std::shared_ptr<FakeTime>& t) {
    return [t] { return t->ms; };
}

// Convenience: a store on its own fake clock, default TTL/caps unless overridden.
std::unique_ptr<RegisterStore> MakeStore(uint8_t hostTag,
                                         const std::shared_ptr<FakeTime>& t,
                                         uint64_t ttlMs = RegisterStore::kDefaultTtlMs,
                                         size_t maxCount = 1024,
                                         size_t maxValueBytes = RegisterStore::kDefaultMaxValueBytes) {
    return std::make_unique<RegisterStore>(MakeHost(hostTag), ttlMs, maxCount, maxValueBytes,
                                           TimeFn(t));
}

RegisterRecord MakeRecord(std::string name, std::string value, Hlc written, Hlc touched,
                          HostId host, uint8_t flags = 0) {
    RegisterRecord r;
    r.name = std::move(name);
    r.value = std::move(value);
    r.written = written;
    r.touched = touched;
    r.originHostId = host;
    r.flags = flags;
    return r;
}

}  // namespace

TEST_CASE("Upsert / Read / overwrite / Delete basics") {
    auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });
    auto s = MakeStore(1, t);

    CHECK(s->Upsert("url", "https://a") == WriteResult::Ok);
    auto r = s->Read("url");
    REQUIRE(r.has_value());
    CHECK(r->value == "https://a");
    CHECK_FALSE(r->IsTombstone());

    CHECK(s->Upsert("url", "https://b") == WriteResult::Ok);
    CHECK(s->Read("url")->value == "https://b");
    CHECK(s->LiveCount() == 1);

    CHECK(s->Delete("url") == DeleteResult::Deleted);
    CHECK_FALSE(s->Read("url").has_value());                 // gone for readers
    CHECK(s->Delete("url") == DeleteResult::NotFound);       // already deleted
    CHECK(s->Delete("never-existed") == DeleteResult::NotFound);
    CHECK(s->LiveCount() == 0);
}

TEST_CASE("name validation") {
    CHECK(IsValidRegisterName("url"));
    CHECK(IsValidRegisterName("a.b_c-1"));
    CHECK(IsValidRegisterName(std::string(64, 'a')));
    CHECK_FALSE(IsValidRegisterName(""));
    CHECK_FALSE(IsValidRegisterName("UPPER"));
    CHECK_FALSE(IsValidRegisterName("has space"));
    CHECK_FALSE(IsValidRegisterName("slash/name"));
    CHECK_FALSE(IsValidRegisterName(std::string(65, 'a')));

    auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });
    auto s = MakeStore(1, t);
    CHECK(s->Upsert("Bad Name", "x") == WriteResult::InvalidName);
    CHECK(s->Upsert("", "mirror") == WriteResult::InvalidName);  // "" reserved; written only via MirrorDefault
}

TEST_CASE("value size cap") {
    auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });
    auto s = MakeStore(1, t, RegisterStore::kDefaultTtlMs, 1024, /*maxValueBytes*/ 8);
    CHECK(s->Upsert("k", "12345678") == WriteResult::Ok);            // exactly at the cap
    CHECK(s->Upsert("k", "123456789") == WriteResult::ValueTooLarge);
}

TEST_CASE("count cap: refuses new registers, allows overwrite, frees on delete") {
    auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });
    auto s = MakeStore(1, t, RegisterStore::kDefaultTtlMs, /*maxCount*/ 3);

    CHECK(s->Upsert("a", "1") == WriteResult::Ok);
    CHECK(s->Upsert("b", "1") == WriteResult::Ok);
    CHECK(s->Upsert("c", "1") == WriteResult::Ok);
    CHECK(s->Upsert("d", "1") == WriteResult::CapExceeded);   // 4th distinct name -> refuse
    CHECK(s->Upsert("a", "2") == WriteResult::Ok);            // overwrite at cap is fine
    CHECK(s->Delete("a") == DeleteResult::Deleted);           // frees a live slot
    CHECK(s->Upsert("d", "1") == WriteResult::Ok);            // now it fits
    CHECK(s->LiveCount() == 3);
}

TEST_CASE("idle TTL expiry with drop-on-access (values and tombstones)") {
    auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });
    const uint64_t ttl = 1000;
    auto s = MakeStore(1, t, ttl);

    CHECK(s->Upsert("a", "1") == WriteResult::Ok);
    CHECK(s->Upsert("b", "1") == WriteResult::Ok);
    CHECK(s->Delete("b") == DeleteResult::Deleted);   // b is now a tombstone
    CHECK(s->PhysicalCount() == 2);                   // value 'a' + tombstone 'b'

    t->ms += ttl + 1;                                 // everything idle past TTL
    CHECK_FALSE(s->Read("a").has_value());            // single-record drop-on-access
    (void)s->List();                                  // whole-map prune
    CHECK(s->PhysicalCount() == 0);                   // physical == live: nothing resident
}

TEST_CASE("Read refreshes touched (LRU); List does not") {
    auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });
    const uint64_t ttl = 1000;
    auto s = MakeStore(1, t, ttl);

    CHECK(s->Upsert("hot", "v") == WriteResult::Ok);
    t->ms += 800;
    REQUIRE(s->Read("hot").has_value());     // refreshes touched
    t->ms += 800;                            // 1600 since write, only 800 since the read
    REQUIRE(s->Read("hot").has_value());     // still alive thanks to the refresh

    CHECK(s->Upsert("cold", "v") == WriteResult::Ok);
    t->ms += ttl + 1;
    (void)s->List();                         // listing must NOT refresh touched
    CHECK_FALSE(s->Read("cold").has_value()); // so 'cold' lapses
}

TEST_CASE("tombstone dominates an older straggler value (delete stays sticky)") {
    auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });
    auto a = MakeStore(1, t);

    CHECK(a->Upsert("k", "v1") == WriteResult::Ok);
    auto v1 = a->Read("k");
    REQUIRE(v1.has_value());
    const Hlc writtenV1 = v1->written;

    CHECK(a->Delete("k") == DeleteResult::Deleted);   // tombstone.written > writtenV1

    // A peer that wrote "stale" BEFORE the delete (older written) reconnects late.
    const RegisterRecord straggler =
        MakeRecord("k", "stale", writtenV1, Hlc{ kEpoch, 0 }, MakeHost(2));
    CHECK_FALSE(a->ApplyRemote(straggler));   // dominated by the tombstone -> no change
    CHECK_FALSE(a->Read("k").has_value());    // stays dead
}

TEST_CASE("MergeRecords is commutative, associative, idempotent") {
    std::mt19937 rng(12345);
    auto randHlc = [&] { return Hlc{ kEpoch + (rng() % 10000), rng() % 8 }; };
    auto randHost = [&] { return MakeHost(static_cast<uint8_t>(1 + (rng() % 4))); };

    // Honor the model invariant: a given (originHostId, written) maps to exactly
    // one value. Deriving value from identity guarantees it.
    auto makeRec = [&] {
        const Hlc w = randHlc();
        const HostId h = randHost();
        const std::string v = "v:" + std::to_string(static_cast<int>(h.data()[0])) + ":" +
                              std::to_string(w.wallMs) + ":" + std::to_string(w.counter);
        return MakeRecord("k", v, w, randHlc(), h);
    };

    for (int iter = 0; iter < 3000; ++iter) {
        const RegisterRecord a = makeRec();
        const RegisterRecord b = makeRec();
        const RegisterRecord c = makeRec();

        CHECK(MergeRecords(a, b) == MergeRecords(b, a));                          // commutative
        CHECK(MergeRecords(MergeRecords(a, b), c) ==
              MergeRecords(a, MergeRecords(b, c)));                              // associative
        CHECK(MergeRecords(a, a) == a);                                          // idempotent
    }
}

TEST_CASE("MergeRecords stays commutative even for forged same-identity duplicates") {
    // Two records sharing (written, host) but differing in value -- a malformed or
    // forged pair the normal invariant forbids. The join must still converge.
    const Hlc w{ kEpoch, 0 };
    const RegisterRecord a = MakeRecord("k", "alpha", w, Hlc{ kEpoch, 1 }, MakeHost(1));
    const RegisterRecord b = MakeRecord("k", "bravo", w, Hlc{ kEpoch, 2 }, MakeHost(1));
    CHECK(MergeRecords(a, b) == MergeRecords(b, a));
}

TEST_CASE("N-replica random schedule converges (SEC: partition -> heal)") {
    for (uint32_t seed = 1; seed <= 8; ++seed) {
        std::mt19937 rng(seed);
        constexpr int N = 5;
        auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });

        std::vector<std::unique_ptr<RegisterStore>> reps;
        for (int i = 0; i < N; ++i) {
            reps.push_back(MakeStore(static_cast<uint8_t>(i + 1), t));
        }

        const std::array<std::string, 4> names{ "a", "b", "c", "d" };
        auto syncInto = [](RegisterStore& dst, RegisterStore& src) {
            for (auto& r : src.SnapshotForSync()) {
                dst.ApplyRemote(r);
            }
        };

        // Random local ops with occasional one-directional partial syncs. Time
        // creeps forward but stays far inside the 90-day TTL, so nothing expires.
        for (int step = 0; step < 400; ++step) {
            t->ms += 1 + (rng() % 5);
            const int who = static_cast<int>(rng() % N);
            const std::string& nm = names[rng() % names.size()];
            const int op = static_cast<int>(rng() % 10);
            if (op < 6) {
                reps[who]->Upsert(nm, "v" + std::to_string(rng()));
            } else if (op < 8) {
                reps[who]->Delete(nm);
            } else {
                const int other = static_cast<int>(rng() % N);
                if (other != who) {
                    syncInto(*reps[who], *reps[other]);
                }
            }
        }

        // Heal: all-to-all anti-entropy until a full round changes nothing.
        int guard = 0;
        for (; guard < 200; ++guard) {
            for (int i = 0; i < N; ++i) {
                for (int j = 0; j < N; ++j) {
                    if (i != j) {
                        syncInto(*reps[i], *reps[j]);
                    }
                }
            }
            const auto ref = reps[0]->SnapshotForSync();
            bool allEqual = true;
            for (int i = 1; i < N && allEqual; ++i) {
                if (reps[i]->SnapshotForSync() != ref) {
                    allEqual = false;
                }
            }
            if (allEqual) {
                break;
            }
        }
        REQUIRE(guard < 200);   // converged

        const auto ref = reps[0]->SnapshotForSync();
        for (int i = 1; i < N; ++i) {
            CHECK(reps[i]->SnapshotForSync() == ref);
        }
    }
}

TEST_CASE("clock skew: HLC ratchet lets a later causal write beat a fast-clock peer") {
    auto tA = std::make_shared<FakeTime>(FakeTime{ kEpoch });                 // A: behind
    auto tB = std::make_shared<FakeTime>(FakeTime{ kEpoch + 1'000'000 });     // B: ~16 min ahead
    auto A = MakeStore(1, tA);
    auto B = MakeStore(2, tB);

    A->Upsert("k", "A1");
    B->Upsert("k", "B1");   // higher wall clock -> higher `written`

    for (auto& r : A->SnapshotForSync()) B->ApplyRemote(r);
    for (auto& r : B->SnapshotForSync()) A->ApplyRemote(r);
    CHECK(A->Read("k")->value == "B1");   // B's fast-clock write wins by HLC
    CHECK(B->Read("k")->value == "B1");

    // A writes again, now that it has WITNESSED B's clock: the ratchet lifts A's
    // HLC above B's despite A's slow wall clock, so the later causal write wins.
    A->Upsert("k", "A2");
    for (auto& r : A->SnapshotForSync()) B->ApplyRemote(r);
    CHECK(B->Read("k")->value == "A2");
}

TEST_CASE("PlanPush selects exactly what a peer is missing or stale on") {
    auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });
    auto A = MakeStore(1, t);
    auto B = MakeStore(2, t);

    CHECK(A->Upsert("shared", "old") == WriteResult::Ok);
    for (auto& r : A->SnapshotForSync()) B->ApplyRemote(r);   // both now hold shared=old

    // Diverge.
    CHECK(A->Upsert("shared", "new") == WriteResult::Ok);     // newer written
    CHECK(A->Upsert("only-a", "1") == WriteResult::Ok);        // B lacks it
    CHECK(B->Upsert("only-b", "1") == WriteResult::Ok);        // A lacks it (must NOT be pushed)

    auto plan = A->PlanPush(B->Digest());
    std::sort(plan.begin(), plan.end());
    CHECK(plan == std::vector<std::string>{ "only-a", "shared" });

    // Apply just the planned records; B converges on A's newer state.
    for (auto& r : A->SnapshotForSync()) {
        if (std::find(plan.begin(), plan.end(), r.name) != plan.end()) {
            B->ApplyRemote(r);
        }
    }
    auto bShared = B->Read("shared");
    REQUIRE(bShared.has_value());
    CHECK(bShared->value == "new");
    auto bOnlyA = B->Read("only-a");
    REQUIRE(bOnlyA.has_value());
    CHECK(bOnlyA->value == "1");

    // A touch-only divergence (same written, fresher touched) is also planned.
    auto t2 = std::make_shared<FakeTime>(FakeTime{ kEpoch + 5000 });
    auto C = MakeStore(3, t2);
    auto D = MakeStore(4, t2);
    CHECK(C->Upsert("k", "v") == WriteResult::Ok);
    for (auto& r : C->SnapshotForSync()) D->ApplyRemote(r);  // identical (written, touched)
    CHECK(C->PlanPush(D->Digest()).empty());                // nothing to push yet
    t2->ms += 10;
    REQUIRE(C->Read("k").has_value());                      // bumps C's touched only
    CHECK(C->PlanPush(D->Digest()) == std::vector<std::string>{ "k" });
}

TEST_CASE("default-constructed store configures via SetLocalHost / SetLimits") {
    RegisterStore s;                       // daemon-style default ctor (real wall clock)
    s.SetLocalHost(MakeHost(7));
    s.SetLimits(RegisterStore::kDefaultTtlMs, /*maxCount*/ 2,
                RegisterStore::kDefaultMaxValueBytes);
    CHECK(s.Upsert("a", "1") == WriteResult::Ok);
    CHECK(s.Upsert("b", "1") == WriteResult::Ok);
    CHECK(s.Upsert("c", "1") == WriteResult::CapExceeded);
    CHECK(s.LiveCount() == 2);
}

TEST_CASE("Upsert rejects the reserved empty name") {
    auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });
    auto s = MakeStore(1, t);
    CHECK(s->Upsert("", "x") == WriteResult::InvalidName);   // "" is mirror-only
}

TEST_CASE("default-register mirror is visible in List but never replicated or capped") {
    auto t = std::make_shared<FakeTime>(FakeTime{ kEpoch });
    auto s = MakeStore(1, t, RegisterStore::kDefaultTtlMs, /*maxCount*/ 1);

    s->MirrorDefault("live clipboard");
    CHECK(s->Upsert("real", "v") == WriteResult::Ok);   // mirror does NOT consume the cap=1 slot
    CHECK(s->LiveCount() == 1);                          // cap counts user registers only

    // List shows both the mirror ("") and the user register.
    const auto list = s->List();
    REQUIRE(list.size() == 2);
    bool sawMirror = false;
    bool sawReal = false;
    for (const auto& r : list) {
        if (r.name.empty()) {
            sawMirror = true;
            CHECK(r.value == "live clipboard");
        } else if (r.name == "real") {
            sawReal = true;
        }
    }
    CHECK(sawMirror);
    CHECK(sawReal);

    // Replication surfaces exclude the mirror entirely.
    CHECK(s->Digest().size() == 1);
    CHECK(s->SnapshotForSync().size() == 1);
    for (const auto& e : s->Digest()) {
        CHECK_FALSE(e.name.empty());
    }

    // A peer can never inject the mirror key.
    const RegisterRecord injected =
        MakeRecord("", "evil", Hlc{ kEpoch, 9 }, Hlc{ kEpoch, 9 }, MakeHost(2));
    CHECK_FALSE(s->ApplyRemote(injected));
}
