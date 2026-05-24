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
static NSString* const kIOSAppGroupIdentifier = @"group.net.clipp.ios";

static NSUserDefaults* GetSettingsStore() {
    static NSUserDefaults* store = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        store = [[NSUserDefaults alloc] initWithSuiteName:kIOSAppGroupIdentifier];
        if (store == nil) {
            store = [NSUserDefaults standardUserDefaults];
        }
    });
    return store;
}

static NSUserDefaults* GetLegacySettingsStore() {
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

static CFStringRef CopySettingsAppIdentifierPrefix() {
    CFTypeRef value = CFBundleGetValueForInfoDictionaryKey(CFBundleGetMainBundle(), CFSTR("AppIdentifierPrefix"));
    if (value != nullptr && CFGetTypeID(value) == CFStringGetTypeID()) {
        CFStringRef prefix = static_cast<CFStringRef>(value);
        if (CFStringGetLength(prefix) > 0 && CFStringFind(prefix, CFSTR("$("), 0).location == kCFNotFound) {
            return static_cast<CFStringRef>(CFRetain(prefix));
        }
    }

    return CFStringCreateWithCString(kCFAllocatorDefault, "2262A4CP8N.", kCFStringEncodingUTF8);
}

static CFStringRef CopySettingsKeychainAccessGroup(CFStringRef suffix) {
    CFStringRef prefix = CopySettingsAppIdentifierPrefix();
    CFStringRef accessGroup = CFStringCreateWithFormat(kCFAllocatorDefault,
                                                       nullptr,
                                                       CFSTR("%@%@"),
                                                       prefix,
                                                       suffix);
    CFRelease(prefix);
    return accessGroup;
}

static CFMutableDictionaryRef CreateHostIDQuery(CFStringRef accessGroup) {
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
    if (accessGroup != nullptr) {
        CFDictionaryAddValue(query, kSecAttrAccessGroup, accessGroup);
    }
    return query;
}

static void DeleteHostIDValue(CFStringRef accessGroup) {
    CFMutableDictionaryRef query = CreateHostIDQuery(accessGroup);
    if (query == nullptr) {
        return;
    }
    SecItemDelete(query);
    CFRelease(query);
}

static bool AddHostIDValue(const unsigned char* data, size_t len, CFStringRef accessGroup) {
    CFMutableDictionaryRef query = CreateHostIDQuery(accessGroup);
    if (query == nullptr) {
        return false;
    }

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

static bool CopyHostIDValue(std::vector<unsigned char>& outValue, CFStringRef accessGroup) {
    CFMutableDictionaryRef query = CreateHostIDQuery(accessGroup);
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

static bool WriteHostIDValue(const unsigned char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }

    CFStringRef sharedAccessGroup = CopySettingsKeychainAccessGroup(CFSTR("net.clipp.ios.shared"));
    CFStringRef legacyAccessGroup = CopySettingsKeychainAccessGroup(CFSTR("net.clipp.ios"));

    DeleteHostIDValue(sharedAccessGroup);
    DeleteHostIDValue(nullptr);
    DeleteHostIDValue(legacyAccessGroup);

    bool written = AddHostIDValue(data, len, sharedAccessGroup);
    if (!written) {
        written = AddHostIDValue(data, len, nullptr);
    }

    if (sharedAccessGroup != nullptr) {
        CFRelease(sharedAccessGroup);
    }
    if (legacyAccessGroup != nullptr) {
        CFRelease(legacyAccessGroup);
    }
    return written;
}

static bool ReadHostIDValue(std::vector<unsigned char>& outValue) {
    CFStringRef sharedAccessGroup = CopySettingsKeychainAccessGroup(CFSTR("net.clipp.ios.shared"));
    CFStringRef legacyAccessGroup = CopySettingsKeychainAccessGroup(CFSTR("net.clipp.ios"));

    bool read = CopyHostIDValue(outValue, sharedAccessGroup)
        || CopyHostIDValue(outValue, nullptr)
        || CopyHostIDValue(outValue, legacyAccessGroup);

    if (sharedAccessGroup != nullptr) {
        CFRelease(sharedAccessGroup);
    }
    if (legacyAccessGroup != nullptr) {
        CFRelease(legacyAccessGroup);
    }
    return read;
}

static id ReadSettingsObject(NSString* key) {
    id value = [GetSettingsStore() objectForKey:key];
    if (value != nil) {
        return value;
    }

    NSUserDefaults* legacyStore = GetLegacySettingsStore();
    if (legacyStore != GetSettingsStore()) {
        return [legacyStore objectForKey:key];
    }
    return nil;
}

static bool SynchronizeSettingsStores() {
    bool ok = [GetSettingsStore() synchronize];
    NSUserDefaults* legacyStore = GetLegacySettingsStore();
    if (legacyStore != GetSettingsStore()) {
        ok = [legacyStore synchronize] && ok;
    }
    return ok;
}
}

bool Settings::ReadStringValue(const wchar_t* valueName, std::string& outValue) {
    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    id value = ReadSettingsObject(key);
    if (value == nil || ![value isKindOfClass:[NSString class]]) {
        return false;
    }

    outValue = std::string([(NSString*)value UTF8String]);
    return true;
}

bool Settings::ReadUint32Value(const wchar_t* valueName, int& outValue) {
    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    id value = ReadSettingsObject(key);
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

    id value = ReadSettingsObject(key);
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

    NSString* stringValue = [NSString stringWithUTF8String:value.c_str()];
    [GetSettingsStore() setObject:stringValue forKey:key];
    NSUserDefaults* legacyStore = GetLegacySettingsStore();
    if (legacyStore != GetSettingsStore()) {
        [legacyStore setObject:stringValue forKey:key];
    }
    return SynchronizeSettingsStores();
}

bool Settings::WriteUint32Value(const wchar_t* valueName, int value) {
    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    [GetSettingsStore() setInteger:value forKey:key];
    NSUserDefaults* legacyStore = GetLegacySettingsStore();
    if (legacyStore != GetSettingsStore()) {
        [legacyStore setInteger:value forKey:key];
    }
    return SynchronizeSettingsStores();
}

bool Settings::WriteUint64Value(const wchar_t* valueName, uint64_t value) {
    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    NSNumber* numberValue = [NSNumber numberWithUnsignedLongLong:value];
    [GetSettingsStore() setObject:numberValue forKey:key];
    NSUserDefaults* legacyStore = GetLegacySettingsStore();
    if (legacyStore != GetSettingsStore()) {
        [legacyStore setObject:numberValue forKey:key];
    }
    return SynchronizeSettingsStores();
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
    NSUserDefaults* legacyStore = GetLegacySettingsStore();
    if (legacyStore != GetSettingsStore()) {
        [legacyStore setObject:blob forKey:key];
    }
    return SynchronizeSettingsStores();
}

bool Settings::ReadBinaryValue(const wchar_t* valueName, std::vector<unsigned char>& outValue) {
    if (IsHostIDValue(valueName)) {
        return ReadHostIDValue(outValue);
    }

    NSString* key = SettingsKey(valueName);
    if (key == nil) {
        return false;
    }

    id value = ReadSettingsObject(key);
    if (value == nil || ![value isKindOfClass:[NSData class]]) {
        return false;
    }

    NSData* data = (NSData*)value;
    outValue.resize(data.length);
    if (!outValue.empty()) {
        std::memcpy(outValue.data(), data.bytes, outValue.size());
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
