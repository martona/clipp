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

    bool ParseHexNetworkKey(const std::string& hex, std::array<unsigned char, KeyManager::NetworkKeySize>& networkKey);
private:
    Settings& settings_;
    bool cacheValid_ = false;
    std::array<unsigned char, NetworkKeySize> cachedNetworkKey_{};
};
