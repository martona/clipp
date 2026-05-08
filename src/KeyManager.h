#pragma once

#include "Settings.h"

#include <array>
#include <string>

class KeyManager {
public:
    static constexpr size_t NetworkKeySize = 32;

    explicit KeyManager(Settings& settings);

    bool SetNetworkKey(const std::array<unsigned char, NetworkKeySize>& networkKey, std::string* errorMessage = nullptr);
    bool GetNetworkKey(std::array<unsigned char, NetworkKeySize>& networkKey, std::string* errorMessage = nullptr);
    bool DeriveNetworkKey(const std::string& password, std::array<unsigned char, 32>& outKey);
    std::wstring GetNetworkKeyHash(const std::array<unsigned char, NetworkKeySize>* networkKey = nullptr);
    void ClearNetworkKey();
    bool HaveNetworkKey();

private:
    Settings& settings_;
    bool cacheValid_ = false;
    std::array<unsigned char, NetworkKeySize> cachedNetworkKey_{};
};

extern KeyManager g_keyManager;
