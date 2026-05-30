#include "KeyManager.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN 
    #include <windows.h>
    #include <wincrypt.h>
#endif

#include <sstream>
#include <vector>
#include <sodium.h>
#include "Logger.h"
#include "platform.h"
#include "utils.h"

#ifdef __APPLE__
    #include <CoreFoundation/CoreFoundation.h>
    #include <Security/Security.h>
    #include <TargetConditionals.h>
    #if !TARGET_OS_IPHONE
        #include "platform/macos/KeyVendIpc.h"
    #endif

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

    static CFMutableDictionaryRef CreateNetworkKeyQuery(CFStringRef account, CFStringRef accessGroup = nullptr) {
        CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (query == nullptr) {
            return nullptr;
        }

        CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
        CFDictionaryAddValue(query, kSecAttrService, CFSTR("net.clipp.app"));
        CFDictionaryAddValue(query, kSecAttrAccount, account);
        if (accessGroup != nullptr) {
            CFDictionaryAddValue(query, kSecAttrAccessGroup, accessGroup);
        }

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

    static CFStringRef CopyNetworkKeyAppIdentifierPrefix() {
#if TARGET_OS_IPHONE
        CFTypeRef value = CFBundleGetValueForInfoDictionaryKey(CFBundleGetMainBundle(), CFSTR("AppIdentifierPrefix"));
        if (value != nullptr && CFGetTypeID(value) == CFStringGetTypeID()) {
            CFStringRef prefix = static_cast<CFStringRef>(value);
            if (CFStringGetLength(prefix) > 0 && CFStringFind(prefix, CFSTR("$("), 0).location == kCFNotFound) {
                return static_cast<CFStringRef>(CFRetain(prefix));
            }
        }

        return CFStringCreateWithCString(kCFAllocatorDefault, "2262A4CP8N.", kCFStringEncodingUTF8);
#else
        return nullptr;
#endif
    }

    static CFStringRef CopyNetworkKeyAccessGroup(CFStringRef suffix) {
#if TARGET_OS_IPHONE
        CFStringRef prefix = CopyNetworkKeyAppIdentifierPrefix();
        CFStringRef accessGroup = CFStringCreateWithFormat(kCFAllocatorDefault,
                                                           nullptr,
                                                           CFSTR("%@%@"),
                                                           prefix,
                                                           suffix);
        CFRelease(prefix);
        return accessGroup;
#else
        (void)suffix;
        return nullptr;
#endif
    }

    static void DeleteNetworkKeyItemInAccessGroup(CFStringRef account, CFStringRef accessGroup) {
        CFMutableDictionaryRef query = CreateNetworkKeyQuery(account, accessGroup);
        if (query == nullptr) {
            return;
        }

        AddNoAuthenticationPrompt(query);
        SetNetworkKeySynchronizable(query, kSecAttrSynchronizableAny);
        SecItemDelete(query);
        CFRelease(query);
    }

    static void DeleteNetworkKeyItem(CFStringRef account) {
        CFStringRef sharedAccessGroup = CopyNetworkKeyAccessGroup(CFSTR("net.clipp.ios.shared"));
        CFStringRef legacyAccessGroup = CopyNetworkKeyAccessGroup(CFSTR("net.clipp.ios"));

        DeleteNetworkKeyItemInAccessGroup(account, sharedAccessGroup);
        DeleteNetworkKeyItemInAccessGroup(account, nullptr);
        DeleteNetworkKeyItemInAccessGroup(account, legacyAccessGroup);

        if (sharedAccessGroup != nullptr) {
            CFRelease(sharedAccessGroup);
        }
        if (legacyAccessGroup != nullptr) {
            CFRelease(legacyAccessGroup);
        }
    }

    static OSStatus CopyNetworkKeyDataInAccessGroup(CFStringRef account, CFStringRef accessGroup, CFTypeRef* outData) {
        CFMutableDictionaryRef query = CreateNetworkKeyQuery(account, accessGroup);
        if (query == nullptr) {
            return errSecAllocate;
        }

        CFDictionaryAddValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionaryAddValue(query, kSecMatchLimit, kSecMatchLimitOne);
        AddNoAuthenticationPrompt(query);
        SetNetworkKeySynchronizable(query, kCFBooleanFalse);

        OSStatus status = SecItemCopyMatching(query, outData);
        CFRelease(query);
        return status;
    }

    static OSStatus CopyNetworkKeyData(CFStringRef account, CFTypeRef* outData) {
        CFStringRef sharedAccessGroup = CopyNetworkKeyAccessGroup(CFSTR("net.clipp.ios.shared"));
        CFStringRef legacyAccessGroup = CopyNetworkKeyAccessGroup(CFSTR("net.clipp.ios"));

        OSStatus status = CopyNetworkKeyDataInAccessGroup(account, sharedAccessGroup, outData);
        if (status != errSecSuccess) {
            status = CopyNetworkKeyDataInAccessGroup(account, nullptr, outData);
        }
        if (status != errSecSuccess) {
            status = CopyNetworkKeyDataInAccessGroup(account, legacyAccessGroup, outData);
        }

        if (sharedAccessGroup != nullptr) {
            CFRelease(sharedAccessGroup);
        }
        if (legacyAccessGroup != nullptr) {
            CFRelease(legacyAccessGroup);
        }
        return status;
    }

    static OSStatus AddNetworkKeyData(CFStringRef account, CFDataRef plainData) {
        CFStringRef sharedAccessGroup = CopyNetworkKeyAccessGroup(CFSTR("net.clipp.ios.shared"));
        CFMutableDictionaryRef addQuery = CreateNetworkKeyQuery(account, sharedAccessGroup);
        if (addQuery == nullptr) {
            if (sharedAccessGroup != nullptr) {
                CFRelease(sharedAccessGroup);
            }
            return errSecAllocate;
        }

        CFDictionaryAddValue(addQuery, kSecValueData, plainData);
        CFDictionaryAddValue(addQuery, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
        SetNetworkKeySynchronizable(addQuery, kCFBooleanFalse);

        OSStatus status = SecItemAdd(addQuery, nullptr);
        CFRelease(addQuery);

        if (status != errSecSuccess && sharedAccessGroup != nullptr) {
            CFMutableDictionaryRef fallbackQuery = CreateNetworkKeyQuery(account);
            if (fallbackQuery != nullptr) {
                CFDictionaryAddValue(fallbackQuery, kSecValueData, plainData);
                CFDictionaryAddValue(fallbackQuery, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
                SetNetworkKeySynchronizable(fallbackQuery, kCFBooleanFalse);
                status = SecItemAdd(fallbackQuery, nullptr);
                CFRelease(fallbackQuery);
            }
        }

        if (sharedAccessGroup != nullptr) {
            CFRelease(sharedAccessGroup);
        }
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
#elif defined(__APPLE__)
    DeleteNetworkKeyItem(CFSTR("NetworkKeyV2"));

    CFDataRef plainData = CFDataCreate(kCFAllocatorDefault, networkKey.data(), networkKey.size());
    if (plainData == nullptr) {
        if (errorMessage != nullptr) *errorMessage = "Failed to create keychain payload";
        return false;
    }

    OSStatus status = AddNetworkKeyData(CFSTR("NetworkKeyV2"), plainData);
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
#else
    // Linux: no OS secret store. Persist the raw 32-byte root key through the
    // Settings file backend (created 0600). Same trust model as an ~/.ssh private
    // key; full-disk encryption covers data at rest. We deliberately skip libsecret
    // -- a headless SSH session usually has no unlocked Secret Service / D-Bus,
    // which is exactly where this build runs. The shared "#ifndef __APPLE__" block
    // below writes encryptedBuffer to settings, so Linux reuses Windows' exact
    // storage path ("encrypted" is a misnomer here; the plumbing is identical).
    std::vector<unsigned char> encryptedBuffer(networkKey.begin(), networkKey.end());
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
#elif defined(__APPLE__)
    CFTypeRef outData = nullptr;
    OSStatus status = CopyNetworkKeyData(CFSTR("NetworkKeyV2"), &outData);

    if (status != errSecSuccess || outData == nullptr) {
        if (outData != nullptr) CFRelease(outData);
#if !TARGET_OS_IPHONE
        if (status == errSecItemNotFound) {
            // Keychain reachable, no key stored. Benign: leave errorMessage empty so
            // callers report "(none)" rather than a read failure, and skip the socket
            // (the GUI reads the same login keychain). NB: over SSH the keychain is
            // *unreachable* and returns an access error, not errSecItemNotFound, so
            // this branch doesn't swallow the SSH case.
            return false;
        }
        // Keychain unreachable in this session (e.g. an SSH / headless login can't
        // open the login keychain). Fall back to the running GUI's mutually
        // authenticated socket. Guarded by IsKeyVendServerActive() so the GUI
        // process -- which serves that socket -- never dials itself.
        std::string socketError;
        if (!clipp::macos::IsKeyVendServerActive()) {
            if (clipp::macos::RequestNetworkKeyOverSocket(rootNetworkKey, &socketError)) {
                const bool cachedFromSocket = CacheDerivedKeysFromRoot(rootNetworkKey, errorMessage);
                sodium_memzero(rootNetworkKey.data(), rootNetworkKey.size());
                return cachedFromSocket;
            }
        }
        g_logger.log(__FUNCTION__, Logger::Level::Debug,
                     "Network key unreadable: keychain OSStatus=%d; socket fallback: %s",
                     static_cast<int>(status),
                     socketError.empty() ? "(not attempted)" : socketError.c_str());
        if (errorMessage != nullptr) {
            *errorMessage = "the login keychain is unavailable in this session";
            if (!socketError.empty()) {
                *errorMessage += "; " + socketError;
            }
        }
        return false;
#else
        if (errorMessage != nullptr) *errorMessage = FormatSecurityError("Failed to read network key from keychain", status);
        return false;
#endif
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
#else
    // Linux: read the raw 32-byte root key back from the Settings file backend.
    std::vector<unsigned char> encryptedBuffer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!settings_.getEncryptedNetworkKey(encryptedBuffer)) {
            if (errorMessage != nullptr) {
                *errorMessage = "Failed to read network key from settings";
            }
            return false;
        }
    }
    if (encryptedBuffer.empty()) {
        // No key stored yet. Benign -- mirrors the macOS errSecItemNotFound branch:
        // leave errorMessage empty so callers report "(none)", not a read failure.
        return false;
    }
    if (encryptedBuffer.size() != NetworkKeySize) {
        sodium_memzero(encryptedBuffer.data(), encryptedBuffer.size());
        if (errorMessage != nullptr) {
            *errorMessage = "Stored key size was not 32 bytes";
        }
        return false;
    }
    std::copy(encryptedBuffer.begin(), encryptedBuffer.end(), rootNetworkKey.begin());
    sodium_memzero(encryptedBuffer.data(), encryptedBuffer.size());
#endif

    const bool cached = CacheDerivedKeysFromRoot(rootNetworkKey, errorMessage);
    sodium_memzero(rootNetworkKey.data(), rootNetworkKey.size());
    return cached;
}

#if defined(__APPLE__) && !TARGET_OS_IPHONE
bool KeyManager::ExportRootKeyFromKeychain(NetworkKey& outKey, std::string* errorMessage) {
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
        if (errorMessage != nullptr) *errorMessage = "Stored key size was not 32 bytes";
        return false;
    }
    std::copy(CFDataGetBytePtr(plainData), CFDataGetBytePtr(plainData) + NetworkKeySize, outKey.begin());
    CFRelease(outData);
    return true;
}
#endif

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

std::string KeyManager::BuildKeyDerivationInput(std::string_view networkName, std::string_view password) {
    const std::string canonicalNetworkName = CanonicalizeKeyDerivationText(networkName);
    const std::string canonicalPassword = CanonicalizeKeyDerivationText(password);
    g_logger.log("BuildKeyDerivationInput",
                 Logger::Level::Debug,
                 "Generating network key input with network name: %s",
                 canonicalNetworkName.c_str());

    std::string input;
    input.reserve(canonicalNetworkName.size() + 1 + canonicalPassword.size());
    input.append(canonicalNetworkName);
    input.push_back('|');
    input.append(canonicalPassword);
    return input;
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
