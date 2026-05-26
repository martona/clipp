cmake_minimum_required(VERSION 3.20)

if (POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

if (NOT DEFINED VCPKG_MANIFEST_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/vcpkg.json")
    set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH "vcpkg manifest directory")
endif()

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

project(clipp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC AND WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if (APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

file(GLOB CLIPP_COMMON_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
    "src/platform/*.h"
)

find_package(unofficial-sodium CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

set(CLIPP_THIRD_PARTY_LIBS
    unofficial-sodium::sodium
    xxHash::xxhash
    zstd::libzstd
)

function(clipp_apply_common_target_settings target_name)
    target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target_name} PRIVATE ${CLIPP_THIRD_PARTY_LIBS})

    if (MSVC)
        target_compile_options(${target_name} PRIVATE /utf-8)
    endif()
endfunction()

if (WIN32)
    enable_language(RC)

    set(CLIPP_WINDOWS_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/windows-$<LOWER_CASE:$<CONFIG>>")

    file(GLOB CLIPP_WIN32_PLATFORM_SOURCES CONFIGURE_DEPENDS
        "src/platform/win32/*.cpp"
        "src/platform/win32/*.h"
    )
    file(GLOB CLIPP_DARKMODE_SOURCES CONFIGURE_DEPENDS
        "src/clipp-win32-darkmode32/*.cpp"
        "src/clipp-win32-darkmode32/*.h"
    )

    add_library(darkmode32 STATIC ${CLIPP_DARKMODE_SOURCES})
    target_include_directories(darkmode32 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(darkmode32 PUBLIC WIN32 UNICODE _UNICODE)
    target_compile_definitions(darkmode32 PRIVATE _LIB)
    target_link_libraries(darkmode32 PUBLIC dwmapi uxtheme Comctl32 Gdi32)
    if (MSVC)
        target_compile_options(darkmode32 PRIVATE /utf-8)
    endif()

    add_executable(clipp-win32 WIN32
        ${CLIPP_COMMON_SOURCES}
        ${CLIPP_WIN32_PLATFORM_SOURCES}
        "src/clipp-win32.rc"
    )
    clipp_apply_common_target_settings(clipp-win32)
    target_compile_definitions(clipp-win32 PRIVATE WIN32 UNICODE _UNICODE _CONSOLE)
    target_link_libraries(clipp-win32 PRIVATE
        darkmode32
        windowsapp
        runtimeobject
        dwmapi
        shell32
        ws2_32
        Crypt32
        Normaliz
        windowscodecs
        dnsapi
    )
    if (MSVC)
        target_link_options(clipp-win32 PRIVATE
            "/ENTRY:mainCRTStartup"
            "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/clipp-win32.exe.manifest"
        )
    endif()

    add_executable(clipp-win32-shim
        "src/clipp-win32-shim/clipp-win32-shim.cpp"
        "src/clipp-win32.rc"
    )
    target_compile_definitions(clipp-win32-shim PRIVATE WIN32 UNICODE _UNICODE _CONSOLE CLIPP_RESOURCE_SHIM)
    if (MSVC)
        target_compile_options(clipp-win32-shim PRIVATE /utf-8)
    endif()
    set_target_properties(clipp-win32-shim PROPERTIES
        OUTPUT_NAME "clipp-win32"
        SUFFIX ".com"
    )

    foreach (target_name IN ITEMS darkmode32 clipp-win32 clipp-win32-shim)
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CLIPP_WINDOWS_OUTPUT_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${CLIPP_WINDOWS_OUTPUT_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY "${CLIPP_WINDOWS_OUTPUT_DIR}"
            PDB_OUTPUT_DIRECTORY "${CLIPP_WINDOWS_OUTPUT_DIR}"
            COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${target_name}"
        )
    endforeach()
elseif (APPLE)
    file(GLOB CLIPP_MACOS_PLATFORM_SOURCES CONFIGURE_DEPENDS
        "src/platform/macos/*.h"
        "src/platform/macos/*.mm"
    )
    # Cross-Apple discovery impl (used on macOS and iOS); add by name since the common glob is
    # restricted to *.cpp / *.h.
    list(APPEND CLIPP_MACOS_PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/MDNSDiscovery_Apple.mm")
    set(CLIPP_MACOS_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/Clipp.icns")
    set(CLIPP_MACOS_MENU_BAR_ICON "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/ClippMenuBarTemplate.png")
    set(CLIPP_MACOS_ABOUT_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/Clipboard4DevAspect-macos-1024-transparent.png")
    set_source_files_properties(
        "${CLIPP_MACOS_APP_ICON}"
        "${CLIPP_MACOS_MENU_BAR_ICON}"
        "${CLIPP_MACOS_ABOUT_IMAGE}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )

    add_executable(clipp MACOSX_BUNDLE
        ${CLIPP_COMMON_SOURCES}
        ${CLIPP_MACOS_PLATFORM_SOURCES}
        "${CLIPP_MACOS_APP_ICON}"
        "${CLIPP_MACOS_MENU_BAR_ICON}"
        "${CLIPP_MACOS_ABOUT_IMAGE}"
    )
    clipp_apply_common_target_settings(clipp)
    target_compile_options(clipp PRIVATE "-Wno-deprecated-declarations")
    # Existing macOS .mm code relies on ARC semantics (no manual retain/release); enable it.
    # The Xcode generator ignores $<COMPILE_LANGUAGE:OBJCXX> compile options reliably, so set
    # the Xcode build setting directly. Keep the compile option for Ninja/Make generators.
    set_target_properties(clipp PROPERTIES XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES")
    target_compile_options(clipp PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")

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
        MACOSX_BUNDLE_ICON_FILE "Clipp.icns"
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
else()
    message(FATAL_ERROR "Unsupported platform.")
endif()
