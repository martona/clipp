#pragma once

#include <cstdint>

#include "platform.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR) && defined(__OBJC__)
#import <UIKit/UIKit.h>
#endif
#endif

enum class OsType : uint16_t {
    Unknown = 0,
    Windows = 1,
    MacOS   = 2,
    IOS_iPhone = 3,
    IOS_iPad = 4,
};

inline OsType GetLocalOsType() {
#if defined(_WIN32)
    return OsType::Windows;
#elif defined(__APPLE__)
    #if TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
        #if defined(__OBJC__)
            return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad
                ? OsType::IOS_iPad
                : OsType::IOS_iPhone;
        #else
            return OsType::IOS_iPhone;
        #endif
    #else
        return OsType::MacOS;
    #endif
#else
    return OsType::Unknown;
#endif
}
