#import "ClippBridge.h"

#include "../../../src/KeyManager.h"
#include "../../../src/Settings.h"
#include "../../../src/platform/uiClippPage.h"

#include <sodium.h>

#include <string>

extern Settings g_settings;
extern KeyManager g_keyManager;

namespace {
constexpr NSInteger kClippNetworkKeyErrorBase = 4100;

NSError* MakeError(NSInteger code, NSString* message) {
    return [NSError errorWithDomain:@"net.clipp.ios.network-key"
                               code:code
                           userInfo:@{ NSLocalizedDescriptionKey: message }];
}

void AssignError(NSError** error, NSInteger code, NSString* message) {
    if (error != nullptr) {
        *error = MakeError(code, message);
    }
}

bool EnsureSodium(NSError** error) {
    static dispatch_once_t once;
    static bool initialized = false;
    dispatch_once(&once, ^{
        initialized = sodium_init() >= 0;
    });

    if (!initialized) {
        AssignError(error, kClippNetworkKeyErrorBase + 1, @"libsodium failed to initialize.");
    }
    return initialized;
}

std::string ToStdString(NSString* value) {
    if (value == nil) {
        return {};
    }

    const char* utf8 = value.UTF8String;
    return utf8 != nullptr ? std::string(utf8) : std::string{};
}

NSString* ToNSString(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data()
                                    length:value.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

NSString* ToNSString(const std::wstring& value) {
    NSMutableString* text = [[NSMutableString alloc] initWithCapacity:value.size()];
    for (wchar_t ch : value) {
        [text appendFormat:@"%C", static_cast<unichar>(ch)];
    }
    return text;
}

CLPNetworkKeyStatus* LoadNetworkKeyStatus(NSError** error) {
    if (!EnsureSodium(error)) {
        return nil;
    }

    NSString* networkName = ToNSString(g_settings.networkName());
    std::string keyError;
    const bool hasKey = g_keyManager.HaveNetworkKey();
    NSString* fingerprint = nil;

    if (hasKey) {
        std::wstring fingerprintText = g_keyManager.GetNetworkFingerprintHash(nullptr, &keyError);
        if (fingerprintText.empty()) {
            AssignError(error,
                        kClippNetworkKeyErrorBase + 2,
                        ToNSString("Unable to read network key fingerprint: " + keyError));
            return nil;
        }
        fingerprint = ToNSString(fingerprintText);
    }

    return [[CLPNetworkKeyStatus alloc] initWithNetworkName:networkName
                                                fingerprint:fingerprint
                                             hasNetworkKey:hasKey];
}
}

@implementation CLPNetworkKeyStatus

- (instancetype)initWithNetworkName:(NSString*)networkName
                         fingerprint:(NSString*)fingerprint
                       hasNetworkKey:(BOOL)hasNetworkKey {
    self = [super init];
    if (self) {
        _networkName = [networkName copy];
        _fingerprint = [fingerprint copy];
        _hasNetworkKey = hasNetworkKey;
    }
    return self;
}

@end

@implementation CLPNetworkKeyBridge

+ (CLPNetworkKeyStatus*)loadStatusWithError:(NSError**)error {
    return LoadNetworkKeyStatus(error);
}

+ (CLPNetworkKeyStatus*)deriveAndStoreKeyWithNetworkName:(NSString*)networkName
                                                  secret:(NSString*)secret
                                                   error:(NSError**)error {
    if (!EnsureSodium(error)) {
        return nil;
    }

    std::string networkNameUtf8 = ToStdString(networkName);
    std::string secretUtf8 = ToStdString(secret);

    if (networkNameUtf8.empty()) {
        AssignError(error, kClippNetworkKeyErrorBase + 3, @"Network name cannot be empty.");
        return nil;
    }

    if (secretUtf8.size() < 8) {
        AssignError(error, kClippNetworkKeyErrorBase + 4, @"Password must be at least 8 characters.");
        sodium_memzero(secretUtf8.data(), secretUtf8.capacity());
        return nil;
    }

    if (!g_settings.set_networkName(networkNameUtf8)) {
        AssignError(error, kClippNetworkKeyErrorBase + 5, @"Unable to store network name.");
        sodium_memzero(secretUtf8.data(), secretUtf8.capacity());
        return nil;
    }

    std::string keyInput = uiClippPage::BuildKeyDerivationInput(networkNameUtf8, secretUtf8);
    sodium_memzero(secretUtf8.data(), secretUtf8.capacity());

    KeyManager::NetworkKey networkKey{};
    const bool derived = g_keyManager.DeriveNetworkKey(keyInput, networkKey);
    sodium_memzero(keyInput.data(), keyInput.capacity());

    if (!derived) {
        AssignError(error, kClippNetworkKeyErrorBase + 6, @"Unable to derive network key.");
        return nil;
    }

    std::string storageError;
    const bool stored = g_keyManager.SetNetworkKey(networkKey, &storageError);
    sodium_memzero(networkKey.data(), networkKey.size());

    if (!stored) {
        AssignError(error,
                    kClippNetworkKeyErrorBase + 7,
                    ToNSString("Unable to store network key: " + storageError));
        return nil;
    }

    return LoadNetworkKeyStatus(error);
}

+ (BOOL)clearNetworkKeyWithError:(NSError**)error {
    if (!EnsureSodium(error)) {
        return NO;
    }

    g_keyManager.ClearNetworkKey();
    return YES;
}

@end
