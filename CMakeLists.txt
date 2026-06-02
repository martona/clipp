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
    option(CLIPP_MACOS_ENABLE_HARDENED_RUNTIME "Enable the macOS hardened runtime (required for notarization)" OFF)
    set(CLIPP_MACOS_DEVELOPMENT_TEAM "" CACHE STRING "Apple development team ID for signing")
    set(CLIPP_MACOS_CODE_SIGN_IDENTITY "" CACHE STRING "Apple code signing identity")
endif()

project(clipp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Version (W.X.Y.Z). Passed as -DCLIPP_VERSION=...; defaults to 0.0.0.0 so plain
# `cmake ...` / `./scripts/build_*` runs without any version flag still configure.
if (NOT DEFINED CLIPP_VERSION OR CLIPP_VERSION STREQUAL "")
    set(CLIPP_VERSION "1.1.1.111")
endif()

string(REPLACE "." ";" _clipp_version_parts "${CLIPP_VERSION}")
list(LENGTH _clipp_version_parts _clipp_version_count)
if (NOT _clipp_version_count EQUAL 4)
    message(FATAL_ERROR
        "CLIPP_VERSION must be W.X.Y.Z (4 dot-separated integers); got '${CLIPP_VERSION}'")
endif()
foreach(_part IN LISTS _clipp_version_parts)
    if (NOT _part MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "CLIPP_VERSION components must be non-negative integers; got '${_part}' in '${CLIPP_VERSION}'")
    endif()
endforeach()
list(GET _clipp_version_parts 0 CLIPP_VERSION_MAJOR)
list(GET _clipp_version_parts 1 CLIPP_VERSION_MINOR)
list(GET _clipp_version_parts 2 CLIPP_VERSION_PATCH)
list(GET _clipp_version_parts 3 CLIPP_VERSION_BUILD)
set(CLIPP_VERSION_3PART "${CLIPP_VERSION_MAJOR}.${CLIPP_VERSION_MINOR}.${CLIPP_VERSION_PATCH}")
message(STATUS "clipp version: ${CLIPP_VERSION} (3-part: ${CLIPP_VERSION_3PART})")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
    @ONLY)

if (MSVC AND WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Use /Z7 (debug info embedded in each .obj) instead of /Zi (separate
    # compile-time vc140.pdb shared across cl.exe instances — that PDB races
    # under parallel ninja and fails with C1041). The linker still produces
    # the final clipp-win32.pdb for the .exe from the embedded info; only the
    # per-translation-unit compile-time PDB goes away. Slight increase in .obj
    # sizes; no contention.
    #
    # Apply to ALL configs (not just Debug) so Release builds also ship a PDB
    # next to the .exe — needed to symbolicate field crashes.
    #
    # FORCE the cache write because CMake caches toolchain-default flags on
    # first configure; without FORCE, a re-configure would keep the old /Zi.
    foreach(flag_var
            CMAKE_CXX_FLAGS_DEBUG          CMAKE_C_FLAGS_DEBUG
            CMAKE_CXX_FLAGS_RELEASE        CMAKE_C_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS_MINSIZEREL     CMAKE_C_FLAGS_MINSIZEREL)
        if(DEFINED ${flag_var})
            if("${${flag_var}}" MATCHES "/Zi")
                string(REGEX REPLACE "/Zi" "/Z7" ${flag_var} "${${flag_var}}")
            elseif(NOT "${${flag_var}}" MATCHES "/Z7")
                string(APPEND ${flag_var} " /Z7")
            endif()
            set(${flag_var} "${${flag_var}}" CACHE STRING "" FORCE)
        endif()
    endforeach()

    # Tell the linker to emit a PDB for non-Debug configs. Debug already
    # implies /DEBUG via CMAKE_*_LINKER_FLAGS_DEBUG; Release/RelWithDebInfo/
    # MinSizeRel do not, so /Z7 alone would just bloat .obj files without
    # producing a usable .pdb.
    #
    # CRITICAL: /DEBUG flips the /OPT default from REF,ICF to NOREF,NOICF — the
    # linker assumes a debug build wants every symbol kept for inspection. CMake's
    # Release default only gets dead-code elimination + COMDAT folding because it
    # ships /INCREMENTAL:NO *without* /DEBUG; appending /DEBUG here silently turns
    # that off and retains all unreferenced code (libsodium/zstd/STL/WinRT bloat
    # the binary by ~1MB+). So /OPT:REF /OPT:ICF must be re-asserted EXPLICITLY
    # alongside /DEBUG. The PDB still symbolicates whatever survives stripping.
    foreach(linker_flag_var
            CMAKE_EXE_LINKER_FLAGS_RELEASE
            CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
            CMAKE_EXE_LINKER_FLAGS_MINSIZEREL
            CMAKE_SHARED_LINKER_FLAGS_RELEASE
            CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO
            CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL)
        if(DEFINED ${linker_flag_var} AND NOT "${${linker_flag_var}}" MATCHES "/DEBUG")
            string(APPEND ${linker_flag_var} " /DEBUG /OPT:REF /OPT:ICF")
            set(${linker_flag_var} "${${linker_flag_var}}" CACHE STRING "" FORCE)
        endif()
    endforeach()
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
find_package(CLI11 CONFIG REQUIRED)

set(CLIPP_THIRD_PARTY_LIBS
    unofficial-sodium::sodium
    xxHash::xxhash
    zstd::libzstd
    CLI11::CLI11   # header-only; lands on clipp-win32 + macOS clipp via the common settings
)

function(clipp_apply_common_target_settings target_name)
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"   # generated version.h lives here
    )
    target_link_libraries(${target_name} PRIVATE ${CLIPP_THIRD_PARTY_LIBS})

    if (MSVC)
        target_compile_options(${target_name} PRIVATE /utf-8)
    endif()
endfunction()

if (WIN32)
    enable_language(RC)

    # The device-type peer glyphs are embedded into the exe from this committed,
    # generated font (see src/clipp-win32.rc.in). It is produced on demand by
    # tools/symbols/build_symbols_font.py, not at build time -- fail early with a
    # clear message rather than a cryptic rc.exe "file not found".
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/ClippSymbols.ttf")
        message(FATAL_ERROR
            "src/resources/ClippSymbols.ttf is missing. Generate it first:\n"
            "    pip install fonttools\n"
            "    python tools/symbols/build_symbols_font.py\n"
            "See BUILDING.md (\"Regenerating the device-type symbol font\").")
    endif()

    set(CLIPP_WINDOWS_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/windows-$<LOWER_CASE:$<CONFIG>>")

    # Substitute @CLIPP_VERSION_*@ into the resource script. The generated .rc
    # references "resource.h" and "resources\\..." relative paths; rc.exe finds
    # them via /i include paths (clipp_apply_common_target_settings adds src/
    # for the main exe; the shim adds it explicitly below).
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/clipp-win32.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/clipp-win32.rc"
        @ONLY)
    set(CLIPP_WIN32_RC "${CMAKE_CURRENT_BINARY_DIR}/generated/clipp-win32.rc")

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
    # We don't use darkmode32's INI-file config path; compile it out (the flag is .cpp-only,
    # never in a header, so PRIVATE). Side benefit: it routes initDarkMode() and the lib's
    # WM_SETTINGCHANGE re-init onto the registry-following (follow-OS) branch instead of the
    # force-dark default -- so the lib no longer has to be corrected from the app.
    target_compile_definitions(darkmode32 PRIVATE _DARKMODE_NO_INI_CONFIG)
    target_link_libraries(darkmode32 PUBLIC dwmapi uxtheme Comctl32 Gdi32)
    if (MSVC)
        target_compile_options(darkmode32 PRIVATE /utf-8)
    endif()

    add_executable(clipp-win32 WIN32
        ${CLIPP_COMMON_SOURCES}
        ${CLIPP_WIN32_PLATFORM_SOURCES}
        "${CLIPP_WIN32_RC}"
        # CMake auto-recognizes .manifest sources and routes them to mt.exe
        # at link time. Replaces a hand-rolled /MANIFEST:EMBED + /MANIFESTINPUT
        # link-option pair that worked in Release (where CMake's MT step is
        # skipped under /INCREMENTAL:NO) but conflicted with the MT step in
        # Debug.
        "src/clipp-win32.exe.manifest"
    )
    clipp_apply_common_target_settings(clipp-win32)
    set_target_properties(clipp-win32 PROPERTIES OUTPUT_NAME "clipp")
    target_compile_definitions(clipp-win32 PRIVATE WIN32 UNICODE _UNICODE _CONSOLE)
    target_link_libraries(clipp-win32 PRIVATE
        darkmode32
        windowsapp
        runtimeobject
        dwmapi
        shell32
        shcore       # CreateStreamOverRandomAccessStream (used to write image
                     # bytes into an InMemoryRandomAccessStream synchronously,
                     # bypassing the DataWriter/IAsyncOperation STA assert).
        ws2_32
        Crypt32
        Normaliz
        windowscodecs
        dwrite       # device-type peer glyphs: load the embedded "Clipp Symbols"
        d2d1         # font from RCDATA and rasterize it (see win32/SymbolGlyphs.cpp).
        dnsapi
        iphlpapi     # GetAdaptersAddresses for the interface-change fingerprint.
        dbghelp      # MiniDumpWriteDump for the on-crash handler.
    )
    if (MSVC)
        target_link_options(clipp-win32 PRIVATE
            "/ENTRY:mainCRTStartup"
            # Emit a link map for binary-size attribution (which objects/libs land in
            # .text, what got folded by /OPT:ICF, what COMDATs survived). Free to
            # produce, lands next to the exe as clipp.map; not shipped in the release.
            "/MAP"
        )
    endif()

    add_executable(clipp-win32-shim
        "src/clipp-win32-shim/clipp-win32-shim.cpp"
        "${CLIPP_WIN32_RC}"
    )
    # The generated .rc lives outside src/, so the rc compiler needs an explicit
    # /i path to find "resource.h" and the resources\\*.{ico,png} references.
    target_include_directories(clipp-win32-shim PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(clipp-win32-shim PRIVATE WIN32 UNICODE _UNICODE _CONSOLE CLIPP_RESOURCE_SHIM)
    if (MSVC)
        target_compile_options(clipp-win32-shim PRIVATE /utf-8)
    endif()
    set_target_properties(clipp-win32-shim PROPERTIES
        OUTPUT_NAME "clipp"
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
    # Event-driven network-change watch (nw_path_monitor); .mm because it needs ARC +
    # Network.framework. Shared with the iOS target (add it there in the Xcode project).
    list(APPEND CLIPP_MACOS_PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/NetworkChangeMonitor_Apple.mm")
    # Cross-Apple compound device-badge renderer (CoreText/CoreGraphics); shared
    # verbatim with the iOS target.
    list(APPEND CLIPP_MACOS_PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/ClippGlyphBadge.mm")
    set(CLIPP_MACOS_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/Clipp.icns")
    set(CLIPP_MACOS_MENU_BAR_ICON "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/ClippMenuBarTemplate.png")
    # Device-type peer glyphs: the committed Nerd Fonts subset, registered at runtime
    # via CTFontManager (see platform/macos/NetworkItem.mm). Regenerate with
    # tools/symbols/build_symbols_font.py.
    set(CLIPP_MACOS_SYMBOLS_FONT "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/ClippSymbols.ttf")

    # About-box artwork: the master is a 1024x1024 transparent PNG (~720KB) -- too big
    # to ship when the About box renders it at ~112px. The app icon (Clipp.icns) is the
    # wrong substitute (it's the opaque rounded-tile, no transparency). So generate a
    # small TRANSPARENT 256px derivative at build time with sips (preinstalled on macOS)
    # and bundle that. No generated binary is committed, and it can't drift from the
    # master. ~720KB -> ~56KB in the .app.
    set(CLIPP_MACOS_ABOUT_MASTER "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/Clipboard4DevAspect-macos-1024-transparent.png")
    set(CLIPP_MACOS_ABOUT_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/generated/ClippAbout.png")
    add_custom_command(
        OUTPUT "${CLIPP_MACOS_ABOUT_IMAGE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND sips -s format png -z 256 256 "${CLIPP_MACOS_ABOUT_MASTER}" --out "${CLIPP_MACOS_ABOUT_IMAGE}"
        DEPENDS "${CLIPP_MACOS_ABOUT_MASTER}"
        COMMENT "Generating 256px About artwork from master PNG (sips)"
        VERBATIM
    )

    set_source_files_properties(
        "${CLIPP_MACOS_APP_ICON}"
        "${CLIPP_MACOS_MENU_BAR_ICON}"
        "${CLIPP_MACOS_ABOUT_IMAGE}"
        "${CLIPP_MACOS_SYMBOLS_FONT}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )

    add_executable(clipp MACOSX_BUNDLE
        ${CLIPP_COMMON_SOURCES}
        ${CLIPP_MACOS_PLATFORM_SOURCES}
        "${CLIPP_MACOS_APP_ICON}"
        "${CLIPP_MACOS_MENU_BAR_ICON}"
        "${CLIPP_MACOS_ABOUT_IMAGE}"
        "${CLIPP_MACOS_SYMBOLS_FONT}"
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
    find_library(NETWORK_FRAMEWORK Network REQUIRED)   # nw_path_monitor (NetworkChangeMonitor)

    target_link_libraries(clipp PRIVATE
        ${COREFOUNDATION_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${SECURITY_FRAMEWORK}
        ${APPKIT_FRAMEWORK}
        ${SERVICEMANAGEMENT_FRAMEWORK}
        ${NETWORK_FRAMEWORK}
    )

    # CMake substitutes ${MACOSX_BUNDLE_*} target properties into Info.plist.in
    # automatically at build time (no separate configure_file needed). The 3-part
    # form is what Apple requires for CFBundleShortVersionString; the 4-part full
    # version goes in CFBundleVersion.
    set_target_properties(clipp PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Clipp"
        MACOSX_BUNDLE_GUI_IDENTIFIER "net.clipp.ios"
        MACOSX_BUNDLE_ICON_FILE "Clipp.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/Info.plist.in"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${CLIPP_VERSION_3PART}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${CLIPP_VERSION}"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "net.clipp.ios"
    )

    if (CMAKE_GENERATOR STREQUAL "Xcode")
        if (CLIPP_MACOS_ENABLE_CODE_SIGNING)
            set_target_properties(clipp PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${CLIPP_MACOS_CODE_SIGN_IDENTITY}"
                XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${CLIPP_MACOS_DEVELOPMENT_TEAM}"
                # Explicit entitlements file. Without this, Xcode auto-injects
                # com.apple.security.get-task-allow=true, which makes notarization
                # fail. The file pins get-task-allow=false; add other entitlements
                # here if/when we need them.
                XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/Clipp.entitlements"
            )
            if (CLIPP_MACOS_ENABLE_HARDENED_RUNTIME)
                # Hardened runtime + secure timestamp are notarization prerequisites.
                # OTHER_CODE_SIGN_FLAGS passes --timestamp through to codesign; the
                # ENABLE_HARDENED_RUNTIME attribute is what adds --options=runtime.
                set_target_properties(clipp PROPERTIES
                    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME "YES"
                    XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--timestamp"
                )
            endif()
        else()
            set_target_properties(clipp PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
            )
        endif()
    endif()
elseif (UNIX AND NOT APPLE)
    # ----------------------------------------------------------------------------
    # Linux: terminal-only build. CLI verbs copy/paste/key/hostid only -- no GUI,
    # no tray, no local clipboard I/O, no background sync daemon. The relay-gateway
    # design means this box reaches the mesh through a running GUI peer; it needs to
    # discover one (Avahi) but runs no daemon of its own. See project_linux_port.
    # ----------------------------------------------------------------------------
    find_package(Threads REQUIRED)
    find_package(PkgConfig REQUIRED)
    # Native libavahi-client for DNS-SD discovery. REQUIRED here means configure
    # fails without the dev package; at runtime a missing/stopped avahi-daemon makes
    # discovery fail loudly (by design -- the cause is then obvious to the user).
    pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)
    # utf8proc gives Linux the Unicode NFC that Win/Mac get from the OS, so a
    # non-ASCII network name/password derives the SAME key across platforms. The
    # vcpkg port also ships a deprecated `unofficial-utf8proc` config; use the
    # canonical one (target: utf8proc::utf8proc).
    find_package(utf8proc CONFIG REQUIRED)

    # The common glob pulls in the whole src/ tree; drop the daemon-only TUs. They
    # reference the platform clipboard (NetworkRuntime.cpp) or the daemon globals
    # g_peerManager / g_peerDisplay (Peer/PeerManager/Listener), neither of which
    # exists in the terminal build. The CLI path (Cli.cpp + OneShotPeer.cpp) is
    # fully isolated from all six. MDNSDiscovery_Win32.cpp stays in the list but its
    # body is `#ifdef _WIN32`, so it compiles to an empty TU here;
    # MDNSDiscovery_Linux.cpp (also in the glob, `#ifdef __linux__`) provides the
    # discovery surface.
    set(CLIPP_LINUX_SOURCES ${CLIPP_COMMON_SOURCES})
    list(REMOVE_ITEM CLIPP_LINUX_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/NetworkRuntime.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Listener.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Peer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/PeerManager.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/PeerDisplay.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ClipboardActivityStore.cpp"
    )

    add_executable(clipp-linux
        ${CLIPP_LINUX_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux/Settings.cpp"
    )
    clipp_apply_common_target_settings(clipp-linux)
    # Drives the terminal-only collapse of main() (no daemon/clipboard/tray) and the
    # omission of the `gui` subcommand in Cli.cpp.
    target_compile_definitions(clipp-linux PRIVATE CLIPP_HEADLESS=1)
    target_link_libraries(clipp-linux PRIVATE
        Threads::Threads
        PkgConfig::AVAHI
        utf8proc::utf8proc
    )
    # Statically link the C++ runtime (libstdc++) and the GCC support lib (libgcc).
    # We build releases on an old-glibc base (Debian 11 / glibc 2.31) for broad
    # compatibility, but that base's libstdc++ is too old for our C++20 binary on
    # NEWER targets -- and conversely a new build host's libstdc++ wouldn't exist on
    # old targets. Baking both in decouples the C++ runtime from the target entirely,
    # leaving only glibc + libavahi as runtime deps. Deliberately NOT fully static:
    # glibc must stay dynamic or NSS breaks getpwuid()/gethostname(), which the
    # Settings default-name and LocalPeerName paths rely on. GCC/Clang only.
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_options(clipp-linux PRIVATE -static-libstdc++ -static-libgcc)

        # Dead-code elimination, the GNU analogue of Windows' /OPT:REF. CMake's Release
        # sets -O2/-O3 -DNDEBUG but does NOT dead-strip, and GNU ld (unlike macOS ld64)
        # keeps everything by default. -ffunction-sections/-fdata-sections + --gc-sections
        # drop unreferenced functions/data. (NOTE: the prebuilt libstdc++.a is not itself
        # function-sectioned, so gc-sections can only drop whole unreferenced object
        # members from it, not individual dead functions -- this is why the headless
        # Linux binary doesn't shrink as much as you'd expect vs the GUI builds.)
        target_compile_options(clipp-linux PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(clipp-linux PRIVATE -Wl,--gc-sections)

        # Release: ship a SMALL stripped binary + a separate full-symbol sidecar
        # (clipp.debug), mirroring the Windows exe/PDB split. CMake's Release omits -g,
        # so we add it to generate DWARF, then objcopy splits it out: the .debug sections
        # only ever live in the sidecar, so the shipped binary stays fully stripped while
        # crash backtraces remain symbolicatable from clipp.debug (gdb/addr2line find it
        # via the embedded .gnu_debuglink). Debug builds are left untouched -- symbols
        # stay in the binary for direct gdb, the idiomatic Linux dev flow.
        if (CMAKE_BUILD_TYPE STREQUAL "Release")
            target_compile_options(clipp-linux PRIVATE -g)
            find_program(CLIPP_OBJCOPY NAMES objcopy llvm-objcopy)
            if (CLIPP_OBJCOPY)
                add_custom_command(TARGET clipp-linux POST_BUILD
                    COMMAND "${CLIPP_OBJCOPY}" --only-keep-debug
                            "$<TARGET_FILE:clipp-linux>" "$<TARGET_FILE:clipp-linux>.debug"
                    COMMAND "${CLIPP_OBJCOPY}" --strip-all "$<TARGET_FILE:clipp-linux>"
                    COMMAND "${CLIPP_OBJCOPY}" --add-gnu-debuglink=clipp.debug
                            "$<TARGET_FILE:clipp-linux>"
                    WORKING_DIRECTORY "$<TARGET_FILE_DIR:clipp-linux>"
                    COMMENT "Splitting debug symbols -> clipp.debug, stripping clipp"
                    VERBATIM
                )
            else()
                message(WARNING
                    "objcopy not found: clipp-linux will ship UNSTRIPPED with no debug sidecar.")
            endif()
        endif()
    endif()
    # Let the binary land at the root of the build tree ($BUILD_DIR/clipp). We
    # deliberately do NOT pin RUNTIME_OUTPUT_DIRECTORY into the source tree the way
    # the Windows target does: build_linux.sh relocates the build tree to local disk
    # when the source is on a CIFS/SMB mount (where CMake's configure_file chmod
    # fails outright), and forcing the artifact back onto the share would defeat that.
    set_target_properties(clipp-linux PROPERTIES OUTPUT_NAME "clipp")
else()
    message(FATAL_ERROR "Unsupported platform.")
endif()
