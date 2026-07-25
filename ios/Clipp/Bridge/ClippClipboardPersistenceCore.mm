// Clipboard/activity persistence + the share-extension inbox — iOS ONLY (the
// desktops keep the clipboard stream RAM-only by design; this file exists only
// in the app target's synchronized group, and the runtime below has no desktop
// analogue on purpose).
//
// Two responsibilities, both consumers of the pure ClipboardPersistence module:
//  * the ACTIVITY SNAPSHOT: mirror of RegisterPersistenceRuntime's shape — a
//    debounced writer thread seals the store's items (newest-first, byte-
//    budgeted) into clipboard-<fp8>.bin in the app container; loaded before the
//    network starts; flushed on backgrounding. RATIONALE (user-ratified): the
//    iOS container is sandboxed + encrypted at rest, so "no greppable plaintext"
//    still holds — process-local storage ≈ RAM there; this fixes
//    share-while-off-mesh history vanishing on the most-offline device.
//  * the SHARE INBOX: ingest the extension's one-file-per-item deferred-share
//    drops from the app group into THIS DEVICE'S history (phone-local by
//    decree — see the comment at the ingest), and delete the files.
#import <Foundation/Foundation.h>

#include "../../../src/ClipboardActivityStore.h"
#include "../../../src/ClipboardPersistence.h"
#include "../../../src/KeyManager.h"
#include "../../../src/Logger.h"
#include "../../../src/NetworkDefs.h"
#include "../../../src/platform/DataPaths.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../../src/ClipboardPersistence.cpp"

extern ClipboardActivityStore g_clipboardActivityStore;
extern KeyManager g_keyManager;
extern Logger g_logger;

namespace {

constexpr auto kWriteDebounce = std::chrono::milliseconds(1500);
// The store's RAM cap defaults to 256 MB — far too much to re-seal per change
// on a phone. Persist newest-first until this encoded-bytes budget is spent;
// older (typically image-heavy) items simply don't survive a relaunch, which
// the 24h history age cap would soon do to them anyway.
constexpr size_t kSnapshotBudgetBytes = 32u * 1024u * 1024u;

std::mutex g_mutex;
std::mutex g_saveMutex;
std::condition_variable g_cv;
std::thread g_writer;
bool g_running = false;
bool g_stop = false;
bool g_dirty = false;
bool g_haveKey = false;
bool g_watcherArmed = false;
std::string g_filePath;  // meaningful only while g_haveKey
ClipboardPersistence::SealKey g_sealKey{};

// First 8 hex chars of the (non-secret) group fingerprint — the same file
// scoping as the register snapshot (see RegisterPersistenceRuntime).
std::string FingerprintTag() {
    const std::wstring fingerprint = g_keyManager.GetNetworkFingerprintHash();
    std::string tag;
    for (size_t i = 0; i < fingerprint.size() && i < 8; ++i) {
        tag.push_back(static_cast<char>(fingerprint[i]));
    }
    return tag;
}

bool ResolveKeyAndPath(ClipboardPersistence::SealKey& outKey, std::string& outPath) {
    KeyManager::NetworkKey subkey{};
    if (!g_keyManager.GetKey(KeyManager::KeyRole::ClipboardStorage, subkey)) {
        return false;
    }
    const std::string tag = FingerprintTag();
    std::string dir;
    if (tag.empty() || !clipp::ResolveStateDirectory(dir)) {
        sodium_memzero(subkey.data(), subkey.size());
        return false;
    }
    std::copy(subkey.begin(), subkey.end(), outKey.begin());
    sodium_memzero(subkey.data(), subkey.size());
    outPath = dir + "/clipboard-" + tag + ".bin";
    return true;
}

// Newest-first payload snapshot under the byte budget. Items whose single
// encoded size would blow the remaining budget are skipped (a huge image must
// not evict ALL of the text history behind it).
std::vector<std::shared_ptr<const ClipboardPayload>> CollectSnapshotPayloads() {
    std::vector<ClipboardActivityItemHeader> headers = g_clipboardActivityStore.Snapshot();
    std::sort(headers.begin(), headers.end(),
              [](const ClipboardActivityItemHeader& a, const ClipboardActivityItemHeader& b) {
                  return a.timestamp > b.timestamp;
              });
    std::vector<std::shared_ptr<const ClipboardPayload>> payloads;
    payloads.reserve(headers.size());
    size_t budget = kSnapshotBudgetBytes;
    for (const auto& header : headers) {
        auto payload = g_clipboardActivityStore.PayloadReference(header.id);
        if (!payload) {
            continue;
        }
        const size_t size = payload->EncodedBytes().size();
        if (size > budget) {
            continue;
        }
        budget -= size;
        payloads.push_back(std::move(payload));
    }
    return payloads;
}

void SaveNow(const std::string& path, const ClipboardPersistence::SealKey& key) {
    std::lock_guard<std::mutex> saveLock(g_saveMutex);
    const auto payloads = CollectSnapshotPayloads();
    if (ClipboardPersistence::SaveSnapshotFile(path, payloads, key)) {
        g_logger.log("ClipboardPersistence", Logger::Level::Debug,
                     "Persisted %zu clipboard item(s).", payloads.size());
    } else {
        g_logger.log("ClipboardPersistence", Logger::Level::Warning,
                     "Failed to persist clipboard history to %s.", path.c_str());
    }
}

// Merge the file into the store: plain Add per item — the store's guid dedup +
// relocate rule make any stale snapshot safe, and nothing here touches the
// pasteboard or the hash guard (these are historical rows, not a paste).
void LoadIntoStore(const std::string& path, const ClipboardPersistence::SealKey& key) {
    std::vector<ClipboardPayload> payloads;
    switch (ClipboardPersistence::LoadSnapshotFile(path, key, payloads)) {
    case ClipboardPersistence::LoadResult::NoFile:
        g_logger.log("ClipboardPersistence", Logger::Level::Debug,
                     "No clipboard snapshot at %s; starting empty.", path.c_str());
        return;
    case ClipboardPersistence::LoadResult::Corrupt:
        g_logger.log("ClipboardPersistence", Logger::Level::Warning,
                     "Clipboard snapshot at %s was unreadable; moved aside, starting empty.",
                     path.c_str());
        return;
    case ClipboardPersistence::LoadResult::Loaded:
        break;
    }
    for (auto& payload : payloads) {
        g_clipboardActivityStore.Add(std::make_shared<const ClipboardPayload>(std::move(payload)));
    }
    g_logger.log("ClipboardPersistence", Logger::Level::Info,
                 "Loaded %zu clipboard item(s) from disk.", payloads.size());
}

void WriterLoop() {
    std::unique_lock<std::mutex> lock(g_mutex);
    while (!g_stop) {
        g_cv.wait(lock, [] { return g_dirty || g_stop; });
        if (g_stop) {
            break;
        }
        g_cv.wait_for(lock, kWriteDebounce, [] { return g_stop; });
        if (!g_haveKey) {
            g_dirty = false;  // RAM-only until pairing
            continue;
        }
        g_dirty = false;
        const std::string path = g_filePath;
        const ClipboardPersistence::SealKey key = g_sealKey;
        lock.unlock();
        SaveNow(path, key);
        lock.lock();
    }
    if (g_dirty && g_haveKey) {
        const std::string path = g_filePath;
        const ClipboardPersistence::SealKey key = g_sealKey;
        lock.unlock();
        SaveNow(path, key);
        lock.lock();
    }
}

void ActivityDirtyWatcher(const ClipboardActivityUpdate&, void*) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_dirty = true;
    }
    g_cv.notify_all();
}

// ---- share-extension inbox --------------------------------------------------

NSString* const kAppGroupIdentifier = @"group.net.clipp.ios";

// <app group>/ShareInbox/<fp8>/ — group-scoped like every other persisted
// artifact, so a stash made under one pairing never even gets TRIED against
// another group's key. The extension writes here (its ONLY shared write); the
// main app ingests and deletes. Layout must stay in lockstep with
// ClippShareBridge.mm's InboxDirectoryPath.
bool InboxDirectoryPath(std::string& outDir, bool create) {
    @autoreleasepool {
        NSURL* container = [NSFileManager.defaultManager
            containerURLForSecurityApplicationGroupIdentifier:kAppGroupIdentifier];
        if (container == nil) {
            return false;
        }
        const std::string tag = FingerprintTag();
        if (tag.empty()) {
            return false;
        }
        NSString* dir = [[container.path stringByAppendingPathComponent:@"ShareInbox"]
            stringByAppendingPathComponent:[NSString stringWithUTF8String:tag.c_str()]];
        if (create
            && ![NSFileManager.defaultManager createDirectoryAtPath:dir
                                        withIntermediateDirectories:YES
                                                         attributes:nil
                                                              error:nil]) {
            return false;
        }
        const char* utf8 = dir.UTF8String;
        if (utf8 == nullptr) {
            return false;
        }
        outDir = utf8;
        return true;
    }
}

}  // namespace

void CLPIOSStartClipboardPersistence() {
    if (g_running) {
        return;
    }

    ClipboardPersistence::SealKey key{};
    std::string path;
    if (ResolveKeyAndPath(key, path)) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_haveKey = true;
            g_filePath = path;
            g_sealKey = key;
        }
        LoadIntoStore(path, key);
        sodium_memzero(key.data(), key.size());
    } else {
        g_logger.log("ClipboardPersistence", Logger::Level::Debug,
                     "No network key yet; clipboard history stays in RAM until pairing.");
    }

    // Armed AFTER the load (the register-persistence echo lesson): loading Adds
    // items, and a pre-armed watcher would mark the store dirty just to re-seal
    // the exact bytes we read. Armed once per process — watchers are
    // append-only and iOS cycles Stop/Start across background/foreground.
    if (!g_watcherArmed) {
        g_watcherArmed = true;
        g_clipboardActivityStore.QueryAndRegister(&ActivityDirtyWatcher, nullptr);
    }

    g_running = true;
    g_stop = false;
    g_writer = std::thread(WriterLoop);
}

void CLPIOSClipboardPersistenceKeyChanged() {
    if (!g_running) {
        return;
    }
    std::string oldPath;
    ClipboardPersistence::SealKey oldKey{};
    bool hadKey = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hadKey = g_haveKey;
        oldPath = g_filePath;
        oldKey = g_sealKey;
    }
    if (hadKey) {
        SaveNow(oldPath, oldKey);  // the old group's file keeps what it had
        sodium_memzero(oldKey.data(), oldKey.size());
    }

    ClipboardPersistence::SealKey key{};
    std::string path;
    const bool haveKey = ResolveKeyAndPath(key, path);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_haveKey = haveKey;
        g_filePath = haveKey ? path : std::string();
        if (haveKey) {
            g_sealKey = key;
        } else {
            sodium_memzero(g_sealKey.data(), g_sealKey.size());
        }
        g_dirty = haveKey;
    }
    if (haveKey && (!hadKey || path != oldPath)) {
        LoadIntoStore(path, key);  // merge the new group's stored history
    }
    sodium_memzero(key.data(), key.size());
    g_cv.notify_all();
}

void CLPIOSStopClipboardPersistence() {
    if (!g_running) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_writer.joinable()) {
        g_writer.join();
    }
    g_running = false;
    sodium_memzero(g_sealKey.data(), g_sealKey.size());
}

// Drain the share extension's deferred drops: decode each, add HISTORY-ONLY
// locally, rebroadcast on the SYNC_REPLAY lane (receivers re-insert the history
// entry and leave their live clipboards alone — the undo-restore pattern), then
// delete the file. Peers that connect later still get the item via the
// zero-anchor SYNC replay, so a dead mesh at ingest time loses nothing.
void CLPIOSIngestShareInbox() {
    std::string dir;
    if (!InboxDirectoryPath(dir, /*create=*/false)) {
        return;  // unpaired, or the group container is unavailable
    }

    KeyManager::NetworkKey subkey{};
    if (!g_keyManager.GetKey(KeyManager::KeyRole::ShareInbox, subkey)) {
        return;
    }
    ClipboardPersistence::SealKey key{};
    std::copy(subkey.begin(), subkey.end(), key.begin());
    sodium_memzero(subkey.data(), subkey.size());

    @autoreleasepool {
        NSString* dirNS = [NSString stringWithUTF8String:dir.c_str()];
        NSArray<NSString*>* entries =
            [NSFileManager.defaultManager contentsOfDirectoryAtPath:dirNS error:nil];
        size_t ingested = 0;
        for (NSString* entry in entries) {
            NSString* fullPath = [dirNS stringByAppendingPathComponent:entry];
            if ([entry hasSuffix:@".corrupt"] || [entry hasSuffix:@".tmp"]) {
                // Quarantined or abandoned partials from earlier passes: sweep.
                [NSFileManager.defaultManager removeItemAtPath:fullPath error:nil];
                continue;
            }
            if (![entry hasSuffix:@".bin"]) {
                continue;
            }
            ClipboardPayload payload;
            const auto result =
                ClipboardPersistence::LoadInboxItemFile(fullPath.UTF8String, key, payload);
            if (result == ClipboardPersistence::LoadResult::Loaded) {
                // PHONE-LOCAL by decree (user call): the deferred share joins
                // this device's history — persisted by the snapshot above — and
                // is NOT actively pushed to the mesh. Active delivery needed a
                // pend-and-retry mechanism living inside the network stack's
                // notification path (it deadlocked the runtime on its first
                // outing), all for an edge case; peers still receive the item
                // if a natural history sync ever pulls it, and the user can
                // always re-share from history. Transport flags are cleared —
                // this is authored history, not replay.
                payload.meta.flags &= ~(NetworkDefs::CLPM_FLAG_SYNC_REPLAY | NetworkDefs::CLPM_FLAG_RELAY);
                g_clipboardActivityStore.Add(std::make_shared<const ClipboardPayload>(std::move(payload)));
                ++ingested;
            }
            // Loaded, Corrupt (already renamed .corrupt — swept next pass), or
            // NoFile (raced another ingest): the .bin itself is done either way.
            [NSFileManager.defaultManager removeItemAtPath:fullPath error:nil];
        }
        if (ingested > 0) {
            g_logger.log("ClipboardPersistence", Logger::Level::Info,
                         "Ingested %zu deferred share(s) from the extension inbox.", ingested);
        }
    }
    sodium_memzero(key.data(), key.size());
}
