#include "KeyManager.h"

#include <windows.h>
#include <wincrypt.h>

#include <sstream>
#include <vector>

namespace {
std::string MakeWin32ErrorMessage(const char* prefix, DWORD errorCode) {
    std::ostringstream out;
    out << prefix << " (Win32 error " << errorCode << ")";
    return out.str();
}
} // namespace

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
