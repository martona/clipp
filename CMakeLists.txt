cmake_minimum_required(VERSION 3.20)

if (APPLE AND (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL ""))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS deployment version" FORCE)
endif()

if (APPLE)
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/src/vcpkg-triplets" CACHE PATH "vcpkg overlay triplet directory")

    if (NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
        execute_process(
            COMMAND uname -m
            OUTPUT_VARIABLE CLIPP_HOST_ARCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if (CLIPP_HOST_ARCH STREQUAL "arm64")
            set(VCPKG_TARGET_TRIPLET "arm64-osx-macos14" CACHE STRING "vcpkg target triplet" FORCE)
        elseif (CLIPP_HOST_ARCH STREQUAL "x86_64")
            set(VCPKG_TARGET_TRIPLET "x64-osx-macos14" CACHE STRING "vcpkg target triplet" FORCE)
        else()
            message(FATAL_ERROR "Unsupported macOS architecture: ${CLIPP_HOST_ARCH}")
        endif()
    endif()

    if (NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "")
        if (VCPKG_TARGET_TRIPLET MATCHES "^arm64-")
            set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture" FORCE)
        elseif (VCPKG_TARGET_TRIPLET MATCHES "^x64-")
            set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "macOS target architecture" FORCE)
        endif()
    endif()

    option(CLIPP_MACOS_ENABLE_CODE_SIGNING "Enable macOS code signing for Xcode generator builds" OFF)
    set(CLIPP_MACOS_DEVELOPMENT_TEAM "" CACHE STRING "Apple development team ID for signing")
    set(CLIPP_MACOS_CODE_SIGN_IDENTITY "" CACHE STRING "Apple code signing identity")
endif()

project(clipp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

file(GLOB SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.mm"
    "src/*.h"
    "src/platform/*.h"
)

if (APPLE)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS
        "src/platform/macos/*.h"
        "src/platform/macos/*.mm"
    )
    list(APPEND SOURCES ${PLATFORM_SOURCES})
elseif (WIN32)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS
        "src/platform/win32/*.cpp"
        "src/platform/win32/*.h"
    )
    list(APPEND SOURCES ${PLATFORM_SOURCES})
endif()

if (APPLE)
    add_executable(clipp MACOSX_BUNDLE ${SOURCES})
else()
    add_executable(clipp ${SOURCES})
endif()

target_include_directories(clipp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if (APPLE)
    target_compile_options(clipp PRIVATE "-Wno-deprecated-declarations")
endif()

find_package(unofficial-sodium CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(lodepng CONFIG REQUIRED)

#find_package(unofficial-sodium CONFIG REQUIRED)
## Force CMake to print the absolute path where it found the configuration, then crash
#message(FATAL_ERROR ">>> GHOST CACHE LOCATED AT: ${unofficial-sodium_DIR} <<<")

target_link_libraries(clipp PRIVATE
    unofficial-sodium::sodium
    xxHash::xxhash
    zstd::libzstd
    lodepng
)

if (WIN32)
    target_link_libraries(clipp PRIVATE windowsapp runtimeobject dwmapi)
    if (MSVC)
        target_link_options(clipp PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/clipp-win32.exe.manifest")
    endif()
endif()


if (APPLE)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(SERVICEMANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)

    target_link_libraries(clipp PRIVATE
        ${COREFOUNDATION_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${SECURITY_FRAMEWORK}
        ${APPKIT_FRAMEWORK}
        ${SERVICEMANAGEMENT_FRAMEWORK}
    )

    set_target_properties(clipp PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Clipp"
        MACOSX_BUNDLE_GUI_IDENTIFIER "net.clipp.app"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/Info.plist.in"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "net.clipp.app"
    )

    if (CMAKE_GENERATOR STREQUAL "Xcode")
        if (CLIPP_MACOS_ENABLE_CODE_SIGNING)
            set_target_properties(clipp PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${CLIPP_MACOS_CODE_SIGN_IDENTITY}"
                XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${CLIPP_MACOS_DEVELOPMENT_TEAM}"
            )
        else()
            set_target_properties(clipp PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
            )
        endif()
    endif()
endif()
