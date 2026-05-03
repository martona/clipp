#pragma once

#include <array>
#include <string>

class KeyManager {
public:
    static constexpr size_t NetworkKeySize = 32;

    bool SetNetworkKey(const std::array<unsigned char, NetworkKeySize>& networkKey, std::string* errorMessage = nullptr);
    bool GetNetworkKey(std::array<unsigned char, NetworkKeySize>& networkKey, std::string* errorMessage = nullptr);

private:
    bool cacheValid_ = false;
    std::array<unsigned char, NetworkKeySize> cachedNetworkKey_{};
};
