#include "RegisterPersistenceRuntime.h"

#include "RegisterConfig.h"

#if CLIPP_REGISTERS_DAEMON

#include "KeyManager.h"
#include "Logger.h"
#include "RegisterPersistence.h"
#include "RegisterStore.h"
#include "platform/DataPaths.h"

#include <sodium.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern RegisterStore g_registerStore;
extern KeyManager g_keyManager;
extern Logger g_logger;

namespace clipp {

namespace {

// Anti-entropy reconnects deliver bursts of hundreds of REGWs and every paste
// bumps `touched`; the debounce turns that into one write per quiet moment.
constexpr auto kWriteDebounce = std::chrono::milliseconds(1500);

std::mutex g_mutex;
// Serializes actual file writes: the writer thread and a key-change flush can
// otherwise race two handles onto the same temp file.
std::mutex g_saveMutex;
std::condition_variable g_cv;
std::thread g_writer;
bool g_running = false;
bool g_stop = false;
bool g_dirty = false;
bool g_haveKey = false;
std::string g_filePath;  // meaningful only while g_haveKey
RegisterPersistence::SealKey g_sealKey{};

// First 8 hex chars of the (non-secret, UI-visible) group fingerprint: scopes
// the file to the group, so a group switch is a clean cutover to a fresh file
// and switching back (or reinstalling and rejoining — derivation is
// deterministic from name+passphrase) brings the old registers back.
std::string FingerprintTag() {
    const std::wstring fingerprint = g_keyManager.GetNetworkFingerprintHash();
    std::string tag;
    for (size_t i = 0; i < fingerprint.size() && i < 8; ++i) {
        tag.push_back(static_cast<char>(fingerprint[i]));
    }
    return tag;
}

// Purpose-bound sealing subkey + group-scoped file path. False while unpaired.
bool ResolveKeyAndPath(RegisterPersistence::SealKey& outKey, std::string& outPath) {
    KeyManager::NetworkKey subkey{};
    if (!g_keyManager.GetKey(KeyManager::KeyRole::RegisterStorage, subkey)) {
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
    outPath = dir + "/registers-" + tag + ".bin";
    return true;
}

void SaveNow(const std::string& path, const RegisterPersistence::SealKey& key) {
    std::lock_guard<std::mutex> saveLock(g_saveMutex);
    const std::vector<RegisterRecord> records = g_registerStore.SnapshotForSync();
    if (RegisterPersistence::SaveSnapshotFile(path, records, key)) {
        g_logger.log("RegisterPersistence", Logger::Level::Debug,
                     "Persisted %zu register record(s).", records.size());
    } else {
        g_logger.log("RegisterPersistence", Logger::Level::Warning,
                     "Failed to persist registers to %s.", path.c_str());
    }
}

// Merge the file into the store. ApplyRemote witnesses both HLCs (lifting the
// clock past every stored stamp) and the CRDT join makes ANY stale snapshot —
// backup restore, VM clone — safe to load.
void LoadIntoStore(const std::string& path, const RegisterPersistence::SealKey& key) {
    std::vector<RegisterRecord> records;
    switch (RegisterPersistence::LoadSnapshotFile(path, key, records)) {
    case RegisterPersistence::LoadResult::NoFile:
        g_logger.log("RegisterPersistence", Logger::Level::Debug,
                     "No register snapshot at %s; starting empty.", path.c_str());
        return;
    case RegisterPersistence::LoadResult::Corrupt:
        g_logger.log("RegisterPersistence", Logger::Level::Warning,
                     "Register snapshot at %s was unreadable; moved aside, starting empty.",
                     path.c_str());
        return;
    case RegisterPersistence::LoadResult::Loaded:
        break;
    }
    size_t applied = 0;
    for (auto& record : records) {
        if (g_registerStore.ApplyRemote(std::move(record))) {
            ++applied;
        }
    }
    g_logger.log("RegisterPersistence", Logger::Level::Info,
                 "Loaded %zu register record(s) from disk (%zu applied).",
                 records.size(), applied);
}

void WriterLoop() {
    std::unique_lock<std::mutex> lock(g_mutex);
    while (!g_stop) {
        g_cv.wait(lock, [] { return g_dirty || g_stop; });
        if (g_stop) {
            break;
        }
        // Absorb the burst; a shutdown request cuts the debounce short.
        g_cv.wait_for(lock, kWriteDebounce, [] { return g_stop; });
        if (!g_haveKey) {
            g_dirty = false;  // RAM-only until pairing; nothing to write
            continue;
        }
        g_dirty = false;
        const std::string path = g_filePath;
        const RegisterPersistence::SealKey key = g_sealKey;
        lock.unlock();
        SaveNow(path, key);
        lock.lock();
    }
    // Final flush on the way out.
    if (g_dirty && g_haveKey) {
        const std::string path = g_filePath;
        const RegisterPersistence::SealKey key = g_sealKey;
        lock.unlock();
        SaveNow(path, key);
        lock.lock();
    }
}

}  // namespace

void StartRegisterPersistence() {
    if (g_running) {
        return;
    }
    // The listener only flips a flag under the runtime mutex, so it is safe
    // from any store-mutating thread (peers, UI, CLI gateway) and stays
    // harmlessly attached after shutdown.
    g_registerStore.SetChangeListener([] {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_dirty = true;
        }
        g_cv.notify_all();
    });

    RegisterPersistence::SealKey key{};
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
        g_logger.log("RegisterPersistence", Logger::Level::Debug,
                     "No network key yet; registers stay in RAM until pairing.");
    }
    g_running = true;
    g_stop = false;
    g_writer = std::thread(WriterLoop);
}

void RegisterPersistenceKeyChanged() {
    if (!g_running) {
        return;
    }
    // Flush what we have into the OLD group's file first — the in-RAM store
    // carries across a pairing change (matching what RAM already does toward
    // the mesh), so nothing is lost on the way out.
    std::string oldPath;
    RegisterPersistence::SealKey oldKey{};
    bool hadKey = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hadKey = g_haveKey;
        oldPath = g_filePath;
        oldKey = g_sealKey;
    }
    if (hadKey) {
        SaveNow(oldPath, oldKey);
        sodium_memzero(oldKey.data(), oldKey.size());
    }

    RegisterPersistence::SealKey key{};
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
        g_dirty = haveKey;  // write the current state into the new file soon
    }
    if (haveKey && (!hadKey || path != oldPath)) {
        LoadIntoStore(path, key);  // merge the new group's stored records
    }
    sodium_memzero(key.data(), key.size());
    g_cv.notify_all();
}

void StopRegisterPersistence() {
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

}  // namespace clipp

#else  // !CLIPP_REGISTERS_DAEMON

namespace clipp {
void StartRegisterPersistence() {}
void RegisterPersistenceKeyChanged() {}
void StopRegisterPersistence() {}
}  // namespace clipp

#endif  // CLIPP_REGISTERS_DAEMON
