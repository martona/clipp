#include "KeyManager.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN 
    #include <windows.h>
    #include <wincrypt.h>
#endif

#include <sstream>
#include <vector>

#ifdef __APPLE__
    #include <CoreFoundation/CoreFoundation.h>
    #include <Security/Security.h>
    static std::string FormatSecurityError(const char* context, OSStatus status) {
        std::ostringstream oss;
        oss << context << " (OSStatus " << status;
        CFStringRef message = SecCopyErrorMessageString(status, nullptr);
        if (message != nullptr) {
            char buffer[256]{};
            if (CFStringGetCString(message, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
                oss << ": " << buffer;
            }
            CFRelease(message);
        }
        oss << ")";
        return oss.str();
    }

    static SecAccessRef CreateAccessForCurrentApp() {
        SecTrustedApplicationRef trustedApp = nullptr;
        OSStatus trustedStatus = SecTrustedApplicationCreateFromPath(nullptr, &trustedApp);
        if (trustedStatus != errSecSuccess || trustedApp == nullptr) {
            return nullptr;
        }

        const void* values[] = { trustedApp };
        CFArrayRef trustedApps = CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
        CFRelease(trustedApp);
        if (trustedApps == nullptr) {
            return nullptr;
        }

        SecAccessRef access = nullptr;
        OSStatus accessStatus = SecAccessCreate(CFSTR("clipp network key"), trustedApps, &access);
        CFRelease(trustedApps);
        if (accessStatus != errSecSuccess) {
            return nullptr;
        }
        return access;
    }
#endif

KeyManager g_keyManager(g_settings);

KeyManager::KeyManager(Settings& settings)
    : settings_(settings) {
}

bool KeyManager::SetNetworkKey(const std::array<unsigned char, NetworkKeySize>& networkKey, std::string* errorMessage) {
#ifdef _WIN32
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
            //TODO
        }
        return false;
    }

    std::vector<unsigned char> encryptedBuffer(encryptedData.pbData, encryptedData.pbData + encryptedData.cbData);
    LocalFree(encryptedData.pbData);
#else
    const CFStringRef service = CFSTR("net.clipp.app");
    const CFStringRef account = CFSTR("NetworkKey");

    CFDataRef plainData = CFDataCreate(kCFAllocatorDefault, networkKey.data(), networkKey.size());
    if (plainData == nullptr) {
        if (errorMessage != nullptr) *errorMessage = "Failed to create keychain payload";
        return false;
    }

    CFMutableDictionaryRef addQuery = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (addQuery == nullptr) {
        CFRelease(plainData);
        if (errorMessage != nullptr) *errorMessage = "Failed to allocate keychain add query";
        return false;
    }

    CFDictionaryAddValue(addQuery, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(addQuery, kSecAttrService, service);
    CFDictionaryAddValue(addQuery, kSecAttrAccount, account);
    CFDictionaryAddValue(addQuery, kSecValueData, plainData);
    CFDictionaryAddValue(addQuery, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
    SecAccessRef access = CreateAccessForCurrentApp();
    if (access != nullptr) {
        CFDictionaryAddValue(addQuery, kSecAttrAccess, access);
    }

    OSStatus status = SecItemAdd(addQuery, nullptr);
    if (access != nullptr) CFRelease(access);
    CFRelease(addQuery);

    if (status == errSecDuplicateItem) {
        CFMutableDictionaryRef matchQuery = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (matchQuery == nullptr) {
            CFRelease(plainData);
            if (errorMessage != nullptr) *errorMessage = "Failed to allocate keychain match query";
            return false;
        }
        CFDictionaryAddValue(matchQuery, kSecClass, kSecClassGenericPassword);
        CFDictionaryAddValue(matchQuery, kSecAttrService, service);
        CFDictionaryAddValue(matchQuery, kSecAttrAccount, account);

        CFMutableDictionaryRef updateAttrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (updateAttrs == nullptr) {
            CFRelease(matchQuery);
            CFRelease(plainData);
            if (errorMessage != nullptr) *errorMessage = "Failed to allocate keychain update query";
            return false;
        }
        CFDictionaryAddValue(updateAttrs, kSecValueData, plainData);
        CFDictionaryAddValue(updateAttrs, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
        SecAccessRef updateAccess = CreateAccessForCurrentApp();
        if (updateAccess != nullptr) {
            CFDictionaryAddValue(updateAttrs, kSecAttrAccess, updateAccess);
        }
        status = SecItemUpdate(matchQuery, updateAttrs);
        if (updateAccess != nullptr) CFRelease(updateAccess);
        CFRelease(updateAttrs);
        CFRelease(matchQuery);
    }

    CFRelease(plainData);

    if (status != errSecSuccess) {
        if (errorMessage != nullptr) {
            *errorMessage = FormatSecurityError("Failed to store network key in keychain", status);
            if (status == errSecInteractionNotAllowed) {
                *errorMessage += ". The login keychain is locked or unavailable in this session; unlock it and retry.";
            }
        }
        return false;
    }

    std::vector<unsigned char> encryptedBuffer;
#endif

#ifndef __APPLE__
    if (!settings_.setEncryptedNetworkKey(encryptedBuffer)) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to write encrypted key to settings";
        }
        return false;
    }
#else
    (void)encryptedBuffer;
#endif

    
    cachedNetworkKey_ = networkKey;
    cacheValid_ = true;
    return true;
}

bool KeyManager::GetNetworkKey(std::array<unsigned char, NetworkKeySize>& networkKey, std::string* errorMessage) {
    if (cacheValid_) {
        networkKey = cachedNetworkKey_;
        return true;
    }

#ifdef _WIN32
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
            //TODO
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
#else
    const CFStringRef service = CFSTR("net.clipp.app");
    const CFStringRef account = CFSTR("NetworkKey");

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (query == nullptr) {
        if (errorMessage != nullptr) *errorMessage = "Failed to allocate keychain query";
        return false;
    }

    CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(query, kSecAttrService, service);
    CFDictionaryAddValue(query, kSecAttrAccount, account);
    CFDictionaryAddValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionaryAddValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFDictionaryAddValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);

    CFTypeRef outData = nullptr;
    OSStatus status = SecItemCopyMatching(query, &outData);
    CFRelease(query);

    if (status != errSecSuccess || outData == nullptr) {
        if (errorMessage != nullptr) *errorMessage = FormatSecurityError("Failed to read network key from keychain", status);
        if (outData != nullptr) CFRelease(outData);
        return false;
    }

    CFDataRef plainData = static_cast<CFDataRef>(outData);
    const CFIndex plainLen = CFDataGetLength(plainData);
    if (plainLen != static_cast<CFIndex>(NetworkKeySize)) {
        CFRelease(outData);
        if (errorMessage != nullptr) {
            *errorMessage = "Decrypted key size was not 32 bytes";
        }
        return false;
    }

    std::copy(CFDataGetBytePtr(plainData), CFDataGetBytePtr(plainData) + NetworkKeySize, networkKey.begin());
    CFRelease(outData);
#endif
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

