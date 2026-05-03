#include "KeyManager.h"

#include <windows.h>
#include <wincrypt.h>

#include <sstream>
#include <vector>

namespace {
constexpr wchar_t kRegistryPath[] = L"Software\\clipp";
constexpr wchar_t kRegistryValueName[] = L"EncryptedNetworkKey";

std::string MakeWin32ErrorMessage(const char* prefix, DWORD errorCode) {
    std::ostringstream out;
    out << prefix << " (Win32 error " << errorCode << ")";
    return out.str();
}
} // namespace

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

    HKEY keyHandle = nullptr;
    LONG registryStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRegistryPath,
        0,
        nullptr,
        0,
        KEY_SET_VALUE,
        nullptr,
        &keyHandle,
        nullptr);

    if (registryStatus != ERROR_SUCCESS) {
        LocalFree(encryptedData.pbData);
        if (errorMessage != nullptr) {
            *errorMessage = MakeWin32ErrorMessage("RegCreateKeyExW failed", registryStatus);
        }
        return false;
    }

    registryStatus = RegSetValueExW(
        keyHandle,
        kRegistryValueName,
        0,
        REG_BINARY,
        encryptedData.pbData,
        encryptedData.cbData);

    RegCloseKey(keyHandle);
    LocalFree(encryptedData.pbData);

    if (registryStatus != ERROR_SUCCESS) {
        if (errorMessage != nullptr) {
            *errorMessage = MakeWin32ErrorMessage("RegSetValueExW failed", registryStatus);
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

    HKEY keyHandle = nullptr;
    LONG registryStatus = RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &keyHandle);
    if (registryStatus != ERROR_SUCCESS) {
        if (errorMessage != nullptr) {
            *errorMessage = MakeWin32ErrorMessage("RegOpenKeyExW failed", registryStatus);
        }
        return false;
    }

    DWORD valueType = 0;
    DWORD encryptedDataSize = 0;
    registryStatus = RegQueryValueExW(keyHandle, kRegistryValueName, nullptr, &valueType, nullptr, &encryptedDataSize);
    if (registryStatus != ERROR_SUCCESS) {
        RegCloseKey(keyHandle);
        if (errorMessage != nullptr) {
            *errorMessage = MakeWin32ErrorMessage("RegQueryValueExW(size) failed", registryStatus);
        }
        return false;
    }

    if (valueType != REG_BINARY || encryptedDataSize == 0) {
        RegCloseKey(keyHandle);
        if (errorMessage != nullptr) {
            *errorMessage = "EncryptedNetworkKey has invalid type or empty data";
        }
        return false;
    }

    std::vector<BYTE> encryptedBuffer(encryptedDataSize);
    registryStatus = RegQueryValueExW(keyHandle, kRegistryValueName, nullptr, nullptr, encryptedBuffer.data(), &encryptedDataSize);
    RegCloseKey(keyHandle);

    if (registryStatus != ERROR_SUCCESS) {
        if (errorMessage != nullptr) {
            *errorMessage = MakeWin32ErrorMessage("RegQueryValueExW(data) failed", registryStatus);
        }
        return false;
    }

    DATA_BLOB encryptedData{};
    encryptedData.pbData = encryptedBuffer.data();
    encryptedData.cbData = encryptedDataSize;

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
