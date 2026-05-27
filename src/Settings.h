#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "HostId.h"

class Settings {
public:
    static constexpr int DefaultMdnsPort = 15353;
    static constexpr int DefaultTcpPort = 15353;
    static constexpr const char* DefaultMulticastIp = "239.255.10.10";
    static constexpr const char* DefaultListenerIp = "0.0.0.0";
    static constexpr uint64_t UnlimitedClipboardHistoryLimit = 0;
    static constexpr uint64_t DefaultClipboardHistoryMemoryLimitBytes = 256ull * 1024ull * 1024ull;
    static constexpr uint64_t DefaultClipboardHistoryMaxAgeSeconds = 24ull * 60ull * 60ull;
    static constexpr uint64_t DefaultClipboardHistoryMaxItems = 1000;
    static constexpr uint64_t DefaultClipboardSyncMaxItems = 30;
    // How many sequence numbers to reserve ahead of the in-memory counter and
    // persist on every flush. A crash loses at most this many numbers; the next
    // session resumes above them, avoiding any collision with the prior session.
    static constexpr uint64_t OriginSequenceBatchSize = 500;
    // Default for honorExternalPrivacyMarkers: respect "don't sync" markers
    // set by other apps (e.g. Chrome / password managers) on the OS clipboard.
    static constexpr bool DefaultHonorExternalPrivacyMarkers = true;

    Settings();

    static bool IsValidPort(int value);
    static bool IsValidListenerIp(const std::string& value);
    static bool IsValidMulticastIp(const std::string& value);

    std::string multicastIp() const;
	std::string listenerIp() const;
    int mdnsPort() const;
    int tcpPort() const;
    std::string networkName() const;
    uint64_t clipboardHistoryMemoryLimitBytes() const;
    uint64_t clipboardHistoryMaxAgeSeconds() const;
    uint64_t clipboardHistoryMaxItems() const;
    uint64_t clipboardSyncMaxItems() const;
    bool honorExternalPrivacyMarkers() const;

    bool set_multicastIp(const std::string& value);
	bool set_listenerIp(const std::string& value);
    bool set_mdnsPort(int value);
    bool set_tcpPort(int value);
    bool set_networkName(const std::string& value);
    bool set_clipboardHistoryMemoryLimitBytes(uint64_t value);
    bool set_clipboardHistoryMaxAgeSeconds(uint64_t value);
    bool set_clipboardHistoryMaxItems(uint64_t value);
    bool set_clipboardSyncMaxItems(uint64_t value);
    bool set_honorExternalPrivacyMarkers(bool value);

    // Atomically increments the per-origin sequence counter and returns the next
    // value. Persists every OriginSequenceBatchSize calls. On startup the counter
    // is pre-bumped by one batch so an unclean shutdown never collides with the
    // next session. Counter is per-origin (this device), monotonic across restarts.
    uint64_t nextOriginSequenceNumber();

    bool setEncryptedNetworkKey(const std::vector<unsigned char>& value);
    bool getEncryptedNetworkKey(std::vector<unsigned char>& value) const;
    bool ensureHostID(HostId& value);
    bool getHostID(HostId& value) const;
    bool resetHostID(HostId& value);

private:
    bool LoadCache();
    static bool ReadStringValue(const wchar_t* valueName, std::string& outValue);
    static bool ReadUint32Value(const wchar_t* valueName, int& outValue);
    static bool ReadUint64Value(const wchar_t* valueName, uint64_t& outValue);
    static bool WriteStringValue(const wchar_t* valueName, const std::string& value);
    static bool WriteUint32Value(const wchar_t* valueName, int value);
    static bool WriteUint64Value(const wchar_t* valueName, uint64_t value);
    static bool WriteBinaryValue(const wchar_t* valueName, const unsigned char* data, size_t len);
    static bool ReadBinaryValue(const wchar_t* valueName, std::vector<unsigned char>& outValue);
    static std::string GetDefaultNetworkName();

    std::string multicastIp_;
	std::string listenerIp_;
    int mdnsPort_;
    int tcpPort_;
    std::string networkName_;
    uint64_t clipboardHistoryMemoryLimitBytes_;
    uint64_t clipboardHistoryMaxAgeSeconds_;
    uint64_t clipboardHistoryMaxItems_;
    uint64_t clipboardSyncMaxItems_;
    bool honorExternalPrivacyMarkers_;
    // In-memory origin sequence counter. Highest value yielded so far.
    uint64_t originSequenceCounter_{ 0 };
    // The next persisted floor — counter values up to (but not including) this
    // are safe to mint without a write. When counter reaches this, we bump the
    // floor by OriginSequenceBatchSize and persist.
    uint64_t originSequencePersistedFloor_{ 0 };
    mutable std::mutex mutex_;
};

extern Settings g_settings;
