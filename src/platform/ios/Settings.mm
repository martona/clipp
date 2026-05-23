#include "../../Settings.h"

#ifdef __APPLE__

#include <cstring>
#include <cwchar>

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <UIKit/UIKit.h>

namespace {
constexpr wchar_t kIOSHostIDName[] = L"HostID";
static const CFStringRef kIOSHostIDService = CFSTR("net.clipp.ios.host");

static NSUserDefaults* GetSettingsStore() {
    return [NSUserDefaults standardUserDefaults];
}

static bool IsHostIDValue(const wchar_t* valueName) {
    return valueName != nullptr && std::wcscmp(valueName, kIOSHostIDName) == 0;
}

static NSString* SettingsKey(const wchar_t* valueName) {
    if (valueName == nullptr) {
        return nil;
    }

    NSMutableString* key = [[NSMutableString alloc] init];
    for (const wchar_t* cursor = valueName; *cursor != L'\0'; ++cursor) {
        [key appendFormat:@"%C", static_cast<unichar>(*cursor)];
    }
    return key;
}

static CFMutableDictionaryRef CreateHostIDQuery() {
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                             0,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    if (query == nullptr) {
        return nullptr;
    }

    CFDictionaryAddValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionaryAddValue(query, kSecAttrService, kIOSHostIDService);
    CFDictionaryAddValue(query, kSecAttrAccount, CFSTR("HostID"));
    CFDictionaryAddValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
    return query;
}

static bool WriteHostIDValue(const unsigned char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }

    CFMutableDictionaryRef query = CreateHostIDQuery();
    if (query == nullptr) {
        return false;
    }
    SecItemDelete(query);

    CFDataRef payload = CFDataCreate(kCFAllocatorDefault, data, len);
    if (payload == nullptr) {
        CFRelease(query);
        return false;
    }

    CFDictionaryAddValue(query, kSecValueData, payload);
    CFDictionaryAddValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
    OSStatus status = SecItemAdd(query, nullptr);

    CFRelease(payload);
    CFRelease(query);
    return status == errSecSuccess;
}

static bool ReadHostIDValue(std::vector<unsigned char>& outValue) {
    CFMutableDictionaryRef query = CreateHostIDQuery();
    if (query == nullptr) {
        return false;
    }

    CFDictionaryAddValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionaryAddValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || result == nullptr) {
        if (result != nullptr) {
            CFRelease(result);
        }
        return false;
    }

    CFDataRef data = static_cast<CFDataRef>(result);
    const CFIndex len = CFDataGetLength(data);
    outValue.resize(static_cast<size_t>(len));
    if (len > 0) {
        std::memcpy(outValue.data(), CFDataGetBytePtr(data), outValue.size());
    }
    CFRelease(result);
    return true;
}
}

bool Settings::ReadStringValue(const wchar_t* valueName, std::string& outValue) {
    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    NSString* value = [GetSettingsStore() stringForKey:key];
    if (value == nil) {
        return false;
    }

    outValue = std::string(value.UTF8String);
    return true;
}

bool Settings::ReadUint32Value(const wchar_t* valueName, int& outValue) {
    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    id value = [GetSettingsStore() objectForKey:key];
    if (value == nil || ![value isKindOfClass:[NSNumber class]]) {
        return false;
    }

    outValue = static_cast<int>([(NSNumber*)value intValue]);
    return true;
}

bool Settings::ReadUint64Value(const wchar_t* valueName, uint64_t& outValue) {
    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    id value = [GetSettingsStore() objectForKey:key];
    if (value == nil || ![value isKindOfClass:[NSNumber class]]) {
        return false;
    }

    outValue = static_cast<uint64_t>([(NSNumber*)value unsignedLongLongValue]);
    return true;
}

bool Settings::WriteStringValue(const wchar_t* valueName, const std::string& value) {
    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    [GetSettingsStore() setObject:[NSString stringWithUTF8String:value.c_str()] forKey:key];
    return [GetSettingsStore() synchronize];
}

bool Settings::WriteUint32Value(const wchar_t* valueName, int value) {
    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    [GetSettingsStore() setInteger:value forKey:key];
    return [GetSettingsStore() synchronize];
}

bool Settings::WriteUint64Value(const wchar_t* valueName, uint64_t value) {
    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    NSNumber* numberValue = [NSNumber numberWithUnsignedLongLong:value];
    [GetSettingsStore() setObject:numberValue forKey:key];
    return [GetSettingsStore() synchronize];
}

bool Settings::WriteBinaryValue(const wchar_t* valueName, const unsigned char* data, size_t len) {
    if (IsHostIDValue(valueName)) {
        return WriteHostIDValue(data, len);
    }

    NSString* key = SettingsKey(valueName);
    if (key == nil || data == nullptr) {
        return false;
    }

    NSData* blob = [NSData dataWithBytes:data length:len];
    [GetSettingsStore() setObject:blob forKey:key];
    return [GetSettingsStore() synchronize];
}

bool Settings::ReadBinaryValue(const wchar_t* valueName, std::vector<unsigned char>& outValue) {
    if (IsHostIDValue(valueName)) {
        return ReadHostIDValue(outValue);
    }

    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    NSData* value = [GetSettingsStore() dataForKey:key];
    if (value == nil) {
        return false;
    }

    outValue.resize(value.length);
    if (!outValue.empty()) {
        std::memcpy(outValue.data(), value.bytes, outValue.size());
    }
    return true;
}

std::string Settings::GetDefaultNetworkName() {
    NSString* deviceName = UIDevice.currentDevice.name;
    if (deviceName.length == 0) {
        deviceName = @"iPhone";
    }

    return std::string(deviceName.UTF8String) + "'s clipp network";
}

#endif
