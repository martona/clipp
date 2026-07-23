// ClipboardActivityStore container tests: guid dedup, MRU relocate, removal by
// wire identity, replay queries, and the watcher protocol. Display-item logic
// (ClipboardActivityDisplay.cpp) is deliberately out of scope -- it depends on
// g_settings and uistrings, which this pure-engine binary does not link.

#include <doctest/doctest.h>

#include "ClipboardActivityStore.h"
#include "ClipboardFormat.h"

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// A minimal wire-shaped payload: meta filled by hand (no StampOrigin -- that
// would pull sodium's RNG), bytes stored via the receive-side entry point so
// no hashing/compression runs. guidByte 0 yields the all-zero "unstamped" guid.
std::shared_ptr<const ClipboardPayload> MakePayload(uint8_t guidByte,
                                                    uint64_t timestampMs,
                                                    const std::string& text = "x") {
    auto payload = std::make_shared<ClipboardPayload>();
    payload->meta.formatId = CLIPP_FORMAT_UTF8;
    payload->meta.timestamp = timestampMs;
    std::memset(payload->meta.eventGuid, 0, sizeof(payload->meta.eventGuid));
    payload->meta.eventGuid[0] = guidByte;
    payload->meta.isCompressed = 0;
    payload->meta.payloadDataSize = text.size();
    payload->meta.uncompressedDataSize = text.size();
    payload->SetEncodedBytes(std::vector<unsigned char>(text.begin(), text.end()));
    return payload;
}

std::array<uint8_t, 16> GuidOf(uint8_t guidByte) {
    std::array<uint8_t, 16> guid{};
    guid[0] = guidByte;
    return guid;
}

struct UpdateLog {
    std::vector<ClipboardActivityUpdate> updates;

    static void Watcher(const ClipboardActivityUpdate& update, void* userData) {
        static_cast<UpdateLog*>(userData)->updates.push_back(update);
    }
};

std::vector<uint8_t> SnapshotGuidBytes(ClipboardActivityStore& store) {
    std::vector<uint8_t> guidBytes;
    for (const auto& header : store.Snapshot()) {
        const auto payload = store.PayloadReference(header.id);
        REQUIRE(payload != nullptr);
        guidBytes.push_back(payload->meta.eventGuid[0]);
    }
    return guidBytes;
}

}  // namespace

TEST_CASE("activity store: add assigns ids and keeps timestamp order") {
    ClipboardActivityStore store;

    const uint64_t idA = store.Add(MakePayload(1, 100));
    const uint64_t idB = store.Add(MakePayload(2, 200));
    const uint64_t idC = store.Add(MakePayload(3, 300));

    CHECK(idA != 0);
    CHECK(idB != 0);
    CHECK(idC != 0);
    CHECK(idA != idB);
    CHECK(idB != idC);

    CHECK(SnapshotGuidBytes(store) == std::vector<uint8_t>{ 1, 2, 3 });
    CHECK(store.TailEventGuid() == GuidOf(3));

    // A sync-replayed historical item slots into its true chronological spot.
    store.Add(MakePayload(4, 150));
    CHECK(SnapshotGuidBytes(store) == std::vector<uint8_t>{ 1, 4, 2, 3 });

    // Null and formatless payloads are not stored.
    CHECK(store.Add(nullptr) == 0);
    CHECK(store.Add(std::make_shared<ClipboardPayload>()) == 0);  // CLIPP_FORMAT_NONE
    CHECK(store.Snapshot().size() == 4);
}

TEST_CASE("activity store: same guid with older-or-equal timestamp is an echo") {
    ClipboardActivityStore store;

    const uint64_t idA = store.Add(MakePayload(1, 100));
    CHECK(store.Add(MakePayload(1, 100)) == idA);  // exact echo
    CHECK(store.Add(MakePayload(1, 50)) == idA);   // stale copy

    CHECK(store.Snapshot().size() == 1);
    const auto payload = store.PayloadReference(idA);
    REQUIRE(payload != nullptr);
    CHECK(payload->meta.timestamp == 100);  // stored payload untouched
}

TEST_CASE("activity store: newer timestamp relocates with a stable id") {
    ClipboardActivityStore store;

    const uint64_t idA = store.Add(MakePayload(1, 100));
    store.Add(MakePayload(2, 200));
    store.Add(MakePayload(3, 300));

    const auto beforeHeaders = store.Snapshot();

    // MRU re-share of A: same guid, bumped origin timestamp.
    CHECK(store.Add(MakePayload(1, 400)) == idA);

    CHECK(store.Snapshot().size() == 3);
    CHECK(SnapshotGuidBytes(store) == std::vector<uint8_t>{ 2, 3, 1 });
    CHECK(store.TailEventGuid() == GuidOf(1));

    // The stored payload is the re-stamped one from now on (replay serves it).
    const auto payload = store.PayloadReference(idA);
    REQUIRE(payload != nullptr);
    CHECK(payload->meta.timestamp == 400);

    // header.timestamp (local add time) refreshed: age tracks the re-share.
    for (const auto& header : store.Snapshot()) {
        if (header.id == idA) {
            CHECK(header.timestamp >= beforeHeaders.front().timestamp);
        }
    }
}

TEST_CASE("activity store: relocate can land mid-stream") {
    ClipboardActivityStore store;

    const uint64_t idA = store.Add(MakePayload(1, 100));
    store.Add(MakePayload(2, 200));
    store.Add(MakePayload(3, 300));

    CHECK(store.Add(MakePayload(1, 250)) == idA);
    CHECK(SnapshotGuidBytes(store) == std::vector<uint8_t>{ 2, 1, 3 });
}

TEST_CASE("activity store: ItemsSince after a relocation") {
    ClipboardActivityStore store;

    store.Add(MakePayload(1, 100));
    store.Add(MakePayload(2, 200));
    store.Add(MakePayload(3, 300));
    store.Add(MakePayload(1, 400));  // move A to the tail

    // Anchor on an unmoved item: everything after it, in order.
    const auto sinceB = store.ItemsSince(GuidOf(2), 10);
    REQUIRE(sinceB.size() == 2);
    CHECK(sinceB[0]->meta.eventGuid[0] == 3);
    CHECK(sinceB[1]->meta.eventGuid[0] == 1);

    // Anchor on the MOVED item: it now sits at the tail, so positional replay
    // yields nothing -- the documented hole that motivates the zero-anchor
    // SYNC on new builds.
    CHECK(store.ItemsSince(GuidOf(1), 10).empty());

    // Zero anchor: the most recent N, which heals the hole (dedup/relocate on
    // the receiving side makes the overlap idempotent).
    const auto all = store.ItemsSince(GuidOf(0), 10);
    REQUIRE(all.size() == 3);
    CHECK(all[0]->meta.eventGuid[0] == 2);
    CHECK(all[1]->meta.eventGuid[0] == 3);
    CHECK(all[2]->meta.eventGuid[0] == 1);

    const auto lastTwo = store.ItemsSince(GuidOf(0), 2);
    REQUIRE(lastTwo.size() == 2);
    CHECK(lastTwo[0]->meta.eventGuid[0] == 3);
    CHECK(lastTwo[1]->meta.eventGuid[0] == 1);
}

TEST_CASE("activity store: all-zero guid never dedups nor relocates") {
    ClipboardActivityStore store;

    const uint64_t first = store.Add(MakePayload(0, 100));
    const uint64_t second = store.Add(MakePayload(0, 200));

    CHECK(first != 0);
    CHECK(second != 0);
    CHECK(first != second);
    CHECK(store.Snapshot().size() == 2);
}

TEST_CASE("activity store: RemoveByEventGuid") {
    ClipboardActivityStore store;

    const uint64_t idA = store.Add(MakePayload(1, 100));
    store.Add(MakePayload(2, 200));
    store.Add(MakePayload(0, 300));  // unstamped item

    CHECK(store.RemoveByEventGuid(GuidOf(1)));
    CHECK(store.Snapshot().size() == 2);
    CHECK(store.PayloadReference(idA) == nullptr);

    // Absent guid: best-effort no-op.
    CHECK_FALSE(store.RemoveByEventGuid(GuidOf(1)));
    // All-zero guid never matches, even though an unstamped item exists.
    CHECK_FALSE(store.RemoveByEventGuid(GuidOf(0)));
    CHECK(store.Snapshot().size() == 2);
}

TEST_CASE("activity store: watcher sees Added, Moved, Removed") {
    ClipboardActivityStore store;
    UpdateLog log;
    const auto registration = store.QueryAndRegister(&UpdateLog::Watcher, &log);
    CHECK(registration.items.empty());

    const uint64_t idA = store.Add(MakePayload(1, 100));
    store.Add(MakePayload(2, 200));
    store.Add(MakePayload(1, 300));  // relocate A
    store.RemoveByEventGuid(GuidOf(2));

    REQUIRE(log.updates.size() == 4);
    CHECK(log.updates[0].type == ClipboardActivityUpdate::Type::Added);
    CHECK(log.updates[1].type == ClipboardActivityUpdate::Type::Added);
    CHECK(log.updates[2].type == ClipboardActivityUpdate::Type::Moved);
    CHECK(log.updates[2].itemID == idA);
    CHECK(log.updates[3].type == ClipboardActivityUpdate::Type::Removed);

    store.Unregister(registration.watcherID);
    store.Add(MakePayload(3, 400));
    CHECK(log.updates.size() == 4);  // unregistered watcher stays silent
}

TEST_CASE("activity store: item cap still evicts the oldest after a relocation") {
    ClipboardActivityStore store;
    store.SetLimits(/*memoryLimitBytes=*/0, /*maxAgeSeconds=*/0, /*maxItems=*/2);

    store.Add(MakePayload(1, 100));
    const uint64_t idB = store.Add(MakePayload(2, 200));
    store.Add(MakePayload(1, 300));  // order: B, A'

    store.Add(MakePayload(3, 400));  // cap 2: evicts B (oldest position)

    CHECK(store.Snapshot().size() == 2);
    CHECK(store.PayloadReference(idB) == nullptr);
    CHECK(SnapshotGuidBytes(store) == std::vector<uint8_t>{ 1, 3 });
}
