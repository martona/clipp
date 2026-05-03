#include "KeyManager.h"

#include <windows.h>
#include <wincrypt.h>

#include <sstream>
#include <vector>

static std::string MakeWin32ErrorMessage(const char* prefix, DWORD errorCode) {
    std::ostringstream out;
    out << prefix << " (Win32 error " << errorCode << ")";
    return out.str();
}

KeyManager::KeyManager(Settings& settings)
    : settings_(settings) {
}

bool KeyManager::SetNetworkKey(const std::array<unsigned char, NetworkKeySize>& networkKey, std::string* errorMessage) {
    DATA_BLOB plainData{};
    plainData.pbData = const_cast<BYTE*>(networkKey.data());
    plainData.cbData = static_cast<DWORD>(networkKey.size());

    DATA_BLOB encryptedData{};
    if (!CryptProtectData(
            &plainData,
            L"clipp network key",
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &encryptedData)) {
        if (errorMessage != nullptr) {
            *errorMessage = MakeWin32ErrorMessage("CryptProtectData failed", GetLastError());
        }
        return false;
    }

    std::vector<unsigned char> encryptedBuffer(encryptedData.pbData, encryptedData.pbData + encryptedData.cbData);
    LocalFree(encryptedData.pbData);

    if (!settings_.setEncryptedNetworkKey(encryptedBuffer)) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to write encrypted key to settings";
        }
        return false;
    }

    cachedNetworkKey_ = networkKey;
    cacheValid_ = true;
    return true;
}

bool KeyManager::GetNetworkKey(std::array<unsigned char, NetworkKeySize>& networkKey, std::string* errorMessage) {
    if (cacheValid_) {
        networkKey = cachedNetworkKey_;
        return true;
    }

    std::vector<unsigned char> encryptedBuffer;
    if (!settings_.getEncryptedNetworkKey(encryptedBuffer)) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to read encrypted key from settings";
        }
        return false;
    }

    DATA_BLOB encryptedData{};
    encryptedData.pbData = encryptedBuffer.data();
    encryptedData.cbData = static_cast<DWORD>(encryptedBuffer.size());

    DATA_BLOB plainData{};
    if (!CryptUnprotectData(
            &encryptedData,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &plainData)) {
        if (errorMessage != nullptr) {
            *errorMessage = MakeWin32ErrorMessage("CryptUnprotectData failed", GetLastError());
        }
        return false;
    }

    if (plainData.cbData != NetworkKeySize) {
        LocalFree(plainData.pbData);
        if (errorMessage != nullptr) {
            *errorMessage = "Decrypted key size was not 32 bytes";
        }
        return false;
    }

    std::copy(plainData.pbData, plainData.pbData + NetworkKeySize, networkKey.begin());
    LocalFree(plainData.pbData);

    cachedNetworkKey_ = networkKey;
    cacheValid_ = true;
    return true;
}

bool KeyManager::ParseHexNetworkKey(const std::string& hex, std::array<unsigned char, KeyManager::NetworkKeySize>& networkKey) {
    if (hex.size() != KeyManager::NetworkKeySize * 2) {
        return false;
    }

    for (size_t i = 0; i < KeyManager::NetworkKeySize; ++i) {
        const std::string byteHex = hex.substr(i * 2, 2);
        char* endPtr = nullptr;
        const long value = std::strtol(byteHex.c_str(), &endPtr, 16);
        if (endPtr == nullptr || *endPtr != '\0' || value < 0 || value > 255) {
            return false;
        }
        networkKey[i] = static_cast<unsigned char>(value);
    }
    return true;
}

