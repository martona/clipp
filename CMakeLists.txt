cmake_minimum_required(VERSION 3.20)

if (APPLE AND (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL ""))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS deployment version" FORCE)
endif()

if (APPLE)
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/src/vcpkg-triplets" CACHE STRING "vcpkg overlay triplet directory" FORCE)
    set(VCPKG_TARGET_TRIPLET "arm64-osx-macos14" CACHE STRING "vcpkg target triplet" FORCE)
    set(CLIPP_MACOS_DEVELOPMENT_TEAM "2262A4CP8N" CACHE STRING "Apple development team ID for signing")
    set(CLIPP_MACOS_CODE_SIGN_IDENTITY "Apple Development" CACHE STRING "macOS code signing identity")
endif()

project(clipp)

set(CMAKE_CXX_STANDARD 20)

file(GLOB SOURCES "src/*.cpp" "src/*.mm" "src/*.h")
list(FILTER SOURCES EXCLUDE REGEX "vcpkg_installed")

if (APPLE)
    list(FILTER SOURCES EXCLUDE REGEX "platform_win32_*")
endif()

if (WIN32)
    list(FILTER SOURCES EXCLUDE REGEX "platform_macos_*")
endif()

if (APPLE)
    add_executable(clipp MACOSX_BUNDLE ${SOURCES})
else()
    add_executable(clipp ${SOURCES})
endif()

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
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/macos/Info.plist.in"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${CLIPP_MACOS_CODE_SIGN_IDENTITY}"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${CLIPP_MACOS_DEVELOPMENT_TEAM}"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "net.clipp.app"
    )
endif()
