#include "Settings.h"
#include "platform.h"
#include <cstring>
#include <cwchar>
#include <unistd.h>
#include <pwd.h>

#ifdef __APPLE__
#import <Foundation/Foundation.h>

namespace {
    static NSString* ToNSString(const wchar_t* valueName) {
        if (valueName == nullptr) {
            return nil;
        }

        const size_t utf8Len = utf16_to_utf8(valueName, std::wcslen(valueName), nullptr, 0);
        if (utf8Len == 0) {
            return @"";
        }

        std::string utf8;
        utf8.resize(utf8Len);
        utf16_to_utf8(valueName, std::wcslen(valueName), utf8.data(), utf8Len);
        return [NSString stringWithUTF8String:utf8.c_str()];
    }

    static NSUserDefaults* GetSettingsStore() {
        return [[NSUserDefaults alloc] initWithSuiteName:@"net.clipp.app"];
    }
}

bool Settings::ReadStringValue(const wchar_t* valueName, std::string& outValue) {
    NSString* key = ToNSString(valueName);
    if (key == nil) {
        return false;
    }

    NSUserDefaults* defaults = GetSettingsStore();
    NSString* value = [defaults stringForKey:key];
    if (value == nil) {
        return false;
    }

    outValue = std::string([value UTF8String]);
    return true;
}

bool Settings::ReadUint32Value(const wchar_t* valueName, int& outValue) {
    NSString* key = ToNSString(valueName);
    if (key == nil) {
        return false;
    }

    NSUserDefaults* defaults = GetSettingsStore();
    id value = [defaults objectForKey:key];
    if (value == nil || ![value isKindOfClass:[NSNumber class]]) {
        return false;
    }

    outValue = static_cast<int>([(NSNumber*)value intValue]);
    return true;
}

bool Settings::ReadUint64Value(const wchar_t* valueName, uint64_t& outValue) {
    NSString* key = ToNSString(valueName);
    if (key == nil) {
        return false;
    }

    NSUserDefaults* defaults = GetSettingsStore();
    id value = [defaults objectForKey:key];
    if (value == nil || ![value isKindOfClass:[NSNumber class]]) {
        return false;
    }

    outValue = static_cast<uint64_t>([(NSNumber*)value unsignedLongLongValue]);
    return true;
}

bool Settings::WriteStringValue(const wchar_t* valueName, const std::string& value) {
    NSString* key = ToNSString(valueName);
    if (key == nil) {
        return false;
    }

    NSUserDefaults* defaults = GetSettingsStore();
    [defaults setObject:[NSString stringWithUTF8String:value.c_str()] forKey:key];
    return [defaults synchronize];
}

bool Settings::WriteUint32Value(const wchar_t* valueName, int value) {
    NSString* key = ToNSString(valueName);
    if (key == nil) {
        return false;
    }

    NSUserDefaults* defaults = GetSettingsStore();
    [defaults setInteger:value forKey:key];
    return [defaults synchronize];
}

bool Settings::WriteUint64Value(const wchar_t* valueName, uint64_t value) {
    NSString* key = ToNSString(valueName);
    if (key == nil) {
        return false;
    }

    NSUserDefaults* defaults = GetSettingsStore();
    NSNumber* numberValue = [NSNumber numberWithUnsignedLongLong:value];
    [defaults setObject:numberValue forKey:key];
    return [defaults synchronize];
}

bool Settings::WriteBinaryValue(const wchar_t* valueName, const unsigned char* data, size_t len) {
    NSString* key = ToNSString(valueName);
    if (key == nil || data == nullptr) {
        return false;
    }

    NSUserDefaults* defaults = GetSettingsStore();
    NSData* blob = [NSData dataWithBytes:data length:len];
    [defaults setObject:blob forKey:key];
    return [defaults synchronize];
}

bool Settings::ReadBinaryValue(const wchar_t* valueName, std::vector<unsigned char>& outValue) {
    NSString* key = ToNSString(valueName);
    if (key == nil) {
        return false;
    }

    NSUserDefaults* defaults = GetSettingsStore();
    NSData* value = [defaults dataForKey:key];
    if (value == nil) {
        return false;
    }

    outValue.resize([value length]);
    if (!outValue.empty()) {
        std::memcpy(outValue.data(), [value bytes], outValue.size());
    }
    return true;
}

std::string Settings::GetDefaultNetworkName() {
    std::string username;

    uid_t uid = getuid();
    struct passwd* pw = getpwuid(uid);
    if (pw && pw->pw_name) {
        username = pw->pw_name;
    }

    if (username.empty()) {
        username = "local";
    }

    return username + "'s clipp network";
}

#endif
