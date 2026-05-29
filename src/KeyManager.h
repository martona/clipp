#pragma once

#include "Settings.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

class KeyManager {
public:
    static constexpr size_t NetworkKeySize = 32;
    static constexpr size_t KeyRoleCount = 4;

    using NetworkKey = std::array<unsigned char, NetworkKeySize>;

    enum class KeyRole : uint64_t {
        TcpHandshakeClientToServer = 1,
        TcpHandshakeServerToClient = 2,
        MDNS = 3,
        Fingerprint = 4,
    };

    explicit KeyManager(Settings& settings);

    bool SetNetworkKey(const NetworkKey& networkKey, std::string* errorMessage = nullptr);
    bool GetKey(KeyRole role, NetworkKey& key, std::string* errorMessage = nullptr);
    bool DeriveNetworkKey(const std::string& password, NetworkKey& outKey);
    // Canonicalizes (Unicode NFC + typographic folding) the network name and
    // password and joins them into the KDF input. Single source of truth shared
    // by the GUI, iOS, and the command line so every surface derives the same key.
    static std::string BuildKeyDerivationInput(std::string_view networkName, std::string_view password);
    std::wstring GetNetworkFingerprintHash(const NetworkKey* networkKey = nullptr, std::string* errorMessage = nullptr);
    void ClearNetworkKey();
    bool HaveNetworkKey();

private:
    using KeyCache = std::array<NetworkKey, KeyRoleCount>;

    bool LoadRootNetworkKey(std::string* errorMessage = nullptr);
    bool CacheDerivedKeysFromRoot(const NetworkKey& rootNetworkKey, std::string* errorMessage = nullptr);

    Settings& settings_;
    bool cacheValid_ = false;
    KeyCache cachedKeys_{};
    mutable std::mutex mutex_;
};

extern KeyManager g_keyManager;
