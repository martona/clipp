#include "KeyManager.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN 
    #include <windows.h>
    #include <wincrypt.h>
#endif

#include <sstream>
#include <vector>
#include <sodium.h>
#include "platform.h"
#include "utils.h"

#ifdef __APPLE__
    #include <CoreFoundation/CoreFoundation.h>
    #include <Security/Security.h>
    #include <limits.h>

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

    static CFMutableDictionaryRef CreateNetworkKeyQuery(CFStringRef account) {
        CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (query == nullptr) {
            return nullptr;
        }

        CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
        CFDictionaryAddValue(query, kSecAttrService, CFSTR("net.clipp.app"));
        CFDictionaryAddValue(query, kSecAttrAccount, account);

        return query;
    }

    static void AddTrustedApplication(CFMutableArrayRef trustedApps, SecTrustedApplicationRef trustedApp) {
        if (trustedApps == nullptr || trustedApp == nullptr) {
            return;
        }

        CFArrayAppendValue(trustedApps, trustedApp);
    }

    static void AddTrustedApplicationFromPath(CFMutableArrayRef trustedApps, const char* path) {
        if (trustedApps == nullptr || path == nullptr || path[0] == '\0') {
            return;
        }

        SecTrustedApplicationRef trustedApp = nullptr;
        if (SecTrustedApplicationCreateFromPath(path, &trustedApp) == errSecSuccess) {
            AddTrustedApplication(trustedApps, trustedApp);
        }

        if (trustedApp != nullptr) {
            CFRelease(trustedApp);
        }
    }

    static void AddTrustedApplicationFromURL(CFMutableArrayRef trustedApps, CFURLRef url) {
        if (trustedApps == nullptr || url == nullptr) {
            return;
        }

        char path[PATH_MAX]{};
        if (CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(path), sizeof(path))) {
            AddTrustedApplicationFromPath(trustedApps, path);
        }
    }

    static SecAccessRef CreateNetworkKeyAccess() {
        CFMutableArrayRef trustedApps = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        if (trustedApps == nullptr) {
            return nullptr;
        }

        SecTrustedApplicationRef currentApp = nullptr;
        if (SecTrustedApplicationCreateFromPath(nullptr, &currentApp) == errSecSuccess) {
            AddTrustedApplication(trustedApps, currentApp);
        }

        if (currentApp != nullptr) {
            CFRelease(currentApp);
        }

        CFBundleRef bundle = CFBundleGetMainBundle();
        if (bundle != nullptr) {
            CFURLRef bundleURL = CFBundleCopyBundleURL(bundle);
            AddTrustedApplicationFromURL(trustedApps, bundleURL);
            if (bundleURL != nullptr) {
                CFRelease(bundleURL);
            }

            CFURLRef executableURL = CFBundleCopyExecutableURL(bundle);
            AddTrustedApplicationFromURL(trustedApps, executableURL);
            if (executableURL != nullptr) {
                CFRelease(executableURL);
            }
        }

        SecAccessRef access = nullptr;
        CFArrayRef trustedList = CFArrayGetCount(trustedApps) > 0 ? trustedApps : nullptr;
        OSStatus status = SecAccessCreate(CFSTR("Clipp network key"), trustedList, &access);
        CFRelease(trustedApps);

        return status == errSecSuccess ? access : nullptr;
    }

    static void AddNoAuthenticationPrompt(CFMutableDictionaryRef query) {
        CFDictionaryAddValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
    }

    static void DeleteNetworkKeyItem(CFStringRef account) {
        CFMutableDictionaryRef query = CreateNetworkKeyQuery(account);
        if (query == nullptr) {
            return;
        }

        AddNoAuthenticationPrompt(query);
        SecItemDelete(query);
        CFRelease(query);
    }

    static OSStatus CopyNetworkKeyData(CFStringRef account, CFTypeRef* outData) {
        CFMutableDictionaryRef query = CreateNetworkKeyQuery(account);
        if (query == nullptr) {
            return errSecAllocate;
        }

        CFDictionaryAddValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionaryAddValue(query, kSecMatchLimit, kSecMatchLimitOne);
        AddNoAuthenticationPrompt(query);

        OSStatus status = SecItemCopyMatching(query, outData);
        CFRelease(query);
        return status;
    }
#endif

static std::vector<unsigned char> HexStringToBytes(const std::string& hex) {
    std::vector<unsigned char> bytes;
    if (hex.size() % 2 != 0)
        return bytes;

    const size_t expected_len = hex.size() / 2;
    bytes.resize(expected_len);

    size_t actual_len = 0;
    int ret = sodium_hex2bin(
        bytes.data(),
        bytes.size(),      // Maximum bytes to write
        hex.c_str(),
        hex.size(),        // Length of the hex string
        nullptr,           // Ignore string (e.g., ": " to ignore colons/spaces)
        &actual_len,       // Outputs the actual number of bytes written
        nullptr            // Pointer to the end of the parsed hex string
    );

    // sodium_hex2bin returns 0 on SUCCESS, -1 on error
    if (ret != 0) {
        bytes.clear();
        return bytes;
    }

    // Shrink the vector to the actual number of bytes parsed
    bytes.resize(actual_len);
    return bytes;
}


KeyManager g_keyManager(g_settings);

KeyManager::KeyManager(Settings& settings)
    : settings_(settings) {
}

void KeyManager::ClearNetworkKey() {
    std::lock_guard<std::mutex> lock(mutex_);
    cacheValid_ = false;
    cachedNetworkKey_.fill(0);
#ifndef __APPLE__
    settings_.setEncryptedNetworkKey(std::vector<unsigned char>{});
#else
    DeleteNetworkKeyItem(CFSTR("NetworkKeyV2"));
#endif
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
    CFDataRef plainData = CFDataCreate(kCFAllocatorDefault, networkKey.data(), networkKey.size());
    if (plainData == nullptr) {
        if (errorMessage != nullptr) *errorMessage = "Failed to create keychain payload";
        return false;
    }

    SecAccessRef access = CreateNetworkKeyAccess();
    if (access == nullptr) {
        CFRelease(plainData);
        if (errorMessage != nullptr) *errorMessage = "Failed to create keychain access list";
        return false;
    }

    CFMutableDictionaryRef addQuery = CreateNetworkKeyQuery(CFSTR("NetworkKeyV2"));
    if (addQuery == nullptr) {
        CFRelease(access);
        CFRelease(plainData);
        if (errorMessage != nullptr) *errorMessage = "Failed to allocate keychain add query";
        return false;
    }

    CFDictionaryAddValue(addQuery, kSecValueData, plainData);
    CFDictionaryAddValue(addQuery, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
    CFDictionaryAddValue(addQuery, kSecAttrAccess, access);

    OSStatus status = SecItemAdd(addQuery, nullptr);
    CFRelease(addQuery);

    if (status == errSecDuplicateItem) {
        CFMutableDictionaryRef matchQuery = CreateNetworkKeyQuery(CFSTR("NetworkKeyV2"));
        if (matchQuery == nullptr) {
            CFRelease(access);
            CFRelease(plainData);
            if (errorMessage != nullptr) *errorMessage = "Failed to allocate keychain match query";
            return false;
        }

        CFMutableDictionaryRef updateAttrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (updateAttrs == nullptr) {
            CFRelease(matchQuery);
            CFRelease(access);
            CFRelease(plainData);
            if (errorMessage != nullptr) *errorMessage = "Failed to allocate keychain update query";
            return false;
        }
        CFDictionaryAddValue(updateAttrs, kSecValueData, plainData);
        CFDictionaryAddValue(updateAttrs, kSecAttrAccess, access);
        status = SecItemUpdate(matchQuery, updateAttrs);
        CFRelease(updateAttrs);
        CFRelease(matchQuery);
    }

    CFRelease(access);
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

    std::lock_guard<std::mutex> lock(mutex_);

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

bool KeyManager::HaveNetworkKey() {
    std::array<unsigned char, NetworkKeySize> networkKey{};
    bool haveKey = GetNetworkKey(networkKey);
    sodium_memzero(networkKey.data(), networkKey.size());
    return haveKey;
}

bool KeyManager::GetNetworkKey(std::array<unsigned char, NetworkKeySize>& networkKey, std::string* errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);

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
    CFTypeRef outData = nullptr;
    OSStatus status = CopyNetworkKeyData(CFSTR("NetworkKeyV2"), &outData);

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

bool KeyManager::DeriveNetworkKey(const std::string& password, std::array<unsigned char, 32>& outKey) {
    static const std::vector<unsigned char> staticSalt = HexStringToBytes("9ea1e55abc07c859fd900958d8b7efbe");
    CLIPP_ASSERT(staticSalt.size() == crypto_pwhash_SALTBYTES);
    if (crypto_pwhash(
        outKey.data(),
        outKey.size(),
        password.c_str(),
        password.length(),
        staticSalt.data(),
        crypto_pwhash_OPSLIMIT_MODERATE,
        crypto_pwhash_MEMLIMIT_MODERATE,
        crypto_pwhash_ALG_ARGON2ID13) != 0) {
        return false;
    }
    return true;
}

static std::wstring FormatHash(const unsigned char* hash, size_t hashLen) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring text;
    text.reserve(hashLen * 2 + hashLen / 4);
    for (std::size_t i = 0; i < hashLen; ++i) {
        if (i > 0 && (i % 4) == 0) {
            text.push_back(L'-');
        }
        text.push_back(digits[(hash[i] >> 4) & 0x0F]);
        text.push_back(digits[hash[i] & 0x0F]);
    }
    return text;
}

std::wstring KeyManager::GetNetworkKeyHash(const std::array<unsigned char, NetworkKeySize>* networkKey) {
    std::array<unsigned char, NetworkKeySize> cachedNetworkKey{};
    if (networkKey == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cacheValid_) {
            cachedNetworkKey = cachedNetworkKey_;
            networkKey = &cachedNetworkKey;
        } else {
            return L"";
        }
    }
    unsigned char keyHash[16];
    crypto_generichash(keyHash, sizeof(keyHash), networkKey->data(), networkKey->size(), nullptr, 0);
    return FormatHash(keyHash, sizeof(keyHash));
}
