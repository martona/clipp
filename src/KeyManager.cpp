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
    #include <TargetConditionals.h>

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

    static void AddNoAuthenticationPrompt(CFMutableDictionaryRef query) {
#if TARGET_OS_IPHONE
        (void)query;
#else
        CFDictionaryAddValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
#endif
    }

    static void SetNetworkKeySynchronizable(CFMutableDictionaryRef query, CFTypeRef value) {
#if TARGET_OS_IPHONE
        if (query != nullptr && value != nullptr) {
            CFDictionarySetValue(query, kSecAttrSynchronizable, value);
        }
#else
        (void)query;
        (void)value;
#endif
    }

    static void DeleteNetworkKeyItem(CFStringRef account) {
        CFMutableDictionaryRef query = CreateNetworkKeyQuery(account);
        if (query == nullptr) {
            return;
        }

        AddNoAuthenticationPrompt(query);
        SetNetworkKeySynchronizable(query, kSecAttrSynchronizableAny);
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
        SetNetworkKeySynchronizable(query, kSecAttrSynchronizableAny);

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

namespace {
    static_assert(KeyManager::NetworkKeySize == crypto_kdf_KEYBYTES);

    constexpr std::array<KeyManager::KeyRole, KeyManager::KeyRoleCount> kKeyRoles = {
        KeyManager::KeyRole::TcpHandshakeClientToServer,
        KeyManager::KeyRole::TcpHandshakeServerToClient,
        KeyManager::KeyRole::MDNS,
        KeyManager::KeyRole::Fingerprint,
    };

    constexpr std::array<std::array<char, crypto_kdf_CONTEXTBYTES>, KeyManager::KeyRoleCount> kKeyRoleContexts = {{
        {'C', 'L', 'P', 'C', '2', 'S', '0', '1'},
        {'C', 'L', 'P', 'S', '2', 'C', '0', '1'},
        {'C', 'L', 'P', 'M', 'D', 'N', 'S', '1'},
        {'C', 'L', 'P', 'F', 'N', 'G', 'R', '1'},
    }};

    size_t KeyRoleIndex(KeyManager::KeyRole role) {
        switch (role) {
        case KeyManager::KeyRole::TcpHandshakeClientToServer:
            return 0;
        case KeyManager::KeyRole::TcpHandshakeServerToClient:
            return 1;
        case KeyManager::KeyRole::MDNS:
            return 2;
        case KeyManager::KeyRole::Fingerprint:
            return 3;
        default:
            return KeyManager::KeyRoleCount;
        }
    }

    bool DeriveKeyFromRoot(
        const KeyManager::NetworkKey& rootNetworkKey,
        KeyManager::KeyRole role,
        KeyManager::NetworkKey& key,
        std::string* errorMessage)
    {
        // Stable libsodium KDF subkey IDs; changing KeyRole values breaks network compatibility.
        if (crypto_kdf_derive_from_key(
                key.data(),
                key.size(),
                static_cast<uint64_t>(role),
                kKeyRoleContexts[KeyRoleIndex(role)].data(),
                rootNetworkKey.data()) != 0) {
            if (errorMessage != nullptr) {
                *errorMessage = "Failed to derive network subkey";
            }
            return false;
        }

        return true;
    }

    bool CalculateKeyCache(
        const KeyManager::NetworkKey& rootNetworkKey,
        std::array<KeyManager::NetworkKey, KeyManager::KeyRoleCount>& keyCache,
        std::string* errorMessage)
    {
        for (KeyManager::KeyRole role : kKeyRoles) {
            if (!DeriveKeyFromRoot(rootNetworkKey, role, keyCache[KeyRoleIndex(role)], errorMessage)) {
                return false;
            }
        }

        return true;
    }

    void ClearKeyCache(std::array<KeyManager::NetworkKey, KeyManager::KeyRoleCount>& keyCache) {
        for (KeyManager::NetworkKey& key : keyCache) {
            sodium_memzero(key.data(), key.size());
        }
    }
}

KeyManager g_keyManager(g_settings);

KeyManager::KeyManager(Settings& settings)
    : settings_(settings) {
}

void KeyManager::ClearNetworkKey() {
    std::lock_guard<std::mutex> lock(mutex_);
    cacheValid_ = false;
    ClearKeyCache(cachedKeys_);
#ifndef __APPLE__
    settings_.setEncryptedNetworkKey(std::vector<unsigned char>{});
#else
    DeleteNetworkKeyItem(CFSTR("NetworkKeyV2"));
#endif
}

bool KeyManager::SetNetworkKey(const NetworkKey& networkKey, std::string* errorMessage) {
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
    DeleteNetworkKeyItem(CFSTR("NetworkKeyV2"));

    CFDataRef plainData = CFDataCreate(kCFAllocatorDefault, networkKey.data(), networkKey.size());
    if (plainData == nullptr) {
        if (errorMessage != nullptr) *errorMessage = "Failed to create keychain payload";
        return false;
    }

    CFMutableDictionaryRef addQuery = CreateNetworkKeyQuery(CFSTR("NetworkKeyV2"));
    if (addQuery == nullptr) {
        CFRelease(plainData);
        if (errorMessage != nullptr) *errorMessage = "Failed to allocate keychain add query";
        return false;
    }

    CFDictionaryAddValue(addQuery, kSecValueData, plainData);
    CFDictionaryAddValue(addQuery, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
#if TARGET_OS_IPHONE
    SetNetworkKeySynchronizable(addQuery, kCFBooleanTrue);
#endif

    OSStatus status = SecItemAdd(addQuery, nullptr);
    CFRelease(addQuery);

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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!settings_.setEncryptedNetworkKey(encryptedBuffer)) {
            if (errorMessage != nullptr) {
                *errorMessage = "Failed to write encrypted key to settings";
            }
            return false;
        }
    }
#else
    (void)encryptedBuffer;
#endif

    return CacheDerivedKeysFromRoot(networkKey, errorMessage);
}

bool KeyManager::HaveNetworkKey() {
    NetworkKey fingerprintKey{};
    bool haveKey = GetKey(KeyRole::Fingerprint, fingerprintKey);
    sodium_memzero(fingerprintKey.data(), fingerprintKey.size());
    return haveKey;
}

bool KeyManager::GetKey(KeyRole role, NetworkKey& key, std::string* errorMessage) {
    const size_t keyIndex = KeyRoleIndex(role);
    if (keyIndex >= KeyRoleCount) {
        if (errorMessage != nullptr) {
            *errorMessage = "Unknown network key role";
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cacheValid_) {
            key = cachedKeys_[keyIndex];
            return true;
        }
    }

    if (!LoadRootNetworkKey(errorMessage)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!cacheValid_) {
        if (errorMessage != nullptr) {
            *errorMessage = "Network key cache was not populated";
        }
        return false;
    }

    key = cachedKeys_[keyIndex];
    return true;
}

bool KeyManager::CacheDerivedKeysFromRoot(const NetworkKey& rootNetworkKey, std::string* errorMessage) {
    KeyCache derivedKeys{};
    if (!CalculateKeyCache(rootNetworkKey, derivedKeys, errorMessage)) {
        ClearKeyCache(derivedKeys);
        std::lock_guard<std::mutex> lock(mutex_);
        cacheValid_ = false;
        ClearKeyCache(cachedKeys_);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ClearKeyCache(cachedKeys_);
        cachedKeys_ = derivedKeys;
        cacheValid_ = true;
    }
    ClearKeyCache(derivedKeys);
    return true;
}

bool KeyManager::LoadRootNetworkKey(std::string* errorMessage) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cacheValid_) {
            return true;
        }
    }

    NetworkKey rootNetworkKey{};

#ifdef _WIN32
    std::vector<unsigned char> encryptedBuffer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!settings_.getEncryptedNetworkKey(encryptedBuffer)) {
            if (errorMessage != nullptr) {
                *errorMessage = "Failed to read encrypted key from settings";
            }
            return false;
        }
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

    std::copy(plainData.pbData, plainData.pbData + NetworkKeySize, rootNetworkKey.begin());
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

    std::copy(CFDataGetBytePtr(plainData), CFDataGetBytePtr(plainData) + NetworkKeySize, rootNetworkKey.begin());
    CFRelease(outData);
#endif

    const bool cached = CacheDerivedKeysFromRoot(rootNetworkKey, errorMessage);
    sodium_memzero(rootNetworkKey.data(), rootNetworkKey.size());
    return cached;
}

bool KeyManager::DeriveNetworkKey(const std::string& password, NetworkKey& outKey) {
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

std::wstring KeyManager::GetNetworkFingerprintHash(const NetworkKey* networkKey, std::string* errorMessage) {
    NetworkKey fingerprintKey{};
    if (networkKey != nullptr) {
        KeyCache keyCache{};
        if (!CalculateKeyCache(*networkKey, keyCache, errorMessage)) {
            ClearKeyCache(keyCache);
            return L"";
        }
        fingerprintKey = keyCache[KeyRoleIndex(KeyRole::Fingerprint)];
        ClearKeyCache(keyCache);
    } else if (!GetKey(KeyRole::Fingerprint, fingerprintKey, errorMessage)) {
        return L"";
    }

    unsigned char keyHash[16];
    crypto_generichash(keyHash, sizeof(keyHash), fingerprintKey.data(), fingerprintKey.size(), nullptr, 0);
    sodium_memzero(fingerprintKey.data(), fingerprintKey.size());
    return FormatHash(keyHash, sizeof(keyHash));
}
