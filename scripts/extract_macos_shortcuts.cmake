# Runs after the Ninja-built executable is linked, before external code signing.
# Xcode runs its own ExtractAppIntentsMetadata phase instead.
execute_process(
    COMMAND xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE clipp_shortcuts_sdk
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
    COMMAND xcrun --find swiftc
    OUTPUT_VARIABLE clipp_shortcuts_swiftc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
get_filename_component(clipp_shortcuts_bin_dir "${clipp_shortcuts_swiftc}" DIRECTORY)
get_filename_component(clipp_shortcuts_toolchain "${clipp_shortcuts_bin_dir}/../.." ABSOLUTE)

if (NOT EXISTS "${CLIPP_SHORTCUTS_CONSTANTS}")
    message(FATAL_ERROR "Swift did not emit App Intents constants: ${CLIPP_SHORTCUTS_CONSTANTS}")
endif()
list(LENGTH CLIPP_SHORTCUTS_ARCH clipp_shortcuts_arch_count)
if (NOT clipp_shortcuts_arch_count EQUAL 1)
    message(FATAL_ERROR "Shortcuts metadata extraction requires a single macOS architecture.")
endif()

set(clipp_shortcuts_output "${CLIPP_APP_BUNDLE}/Contents/Resources")
# An unsuccessful extraction must not accidentally pass using old metadata.
file(REMOVE_RECURSE "${clipp_shortcuts_output}/Metadata.appintents")
execute_process(
    COMMAND xcrun appintentsmetadataprocessor
        --module-name Clipp
        --toolchain-dir "${clipp_shortcuts_toolchain}"
        --sdk-root "${clipp_shortcuts_sdk}"
        --platform-family macOS
        --deployment-target "${CLIPP_SHORTCUTS_DEPLOYMENT_TARGET}"
        --target-triple "${CLIPP_SHORTCUTS_ARCH}-apple-macos${CLIPP_SHORTCUTS_DEPLOYMENT_TARGET}"
        --source-files "${CLIPP_SHORTCUTS_SOURCE}"
        --binary-file "${CLIPP_APP_BINARY}"
        --swift-const-vals "${CLIPP_SHORTCUTS_CONSTANTS}"
        --compile-time-extraction
        --output "${clipp_shortcuts_output}"
    COMMAND_ERROR_IS_FATAL ANY
)

# The processor has historically returned success even when no metadata was
# emitted. Treat that as a build failure instead of shipping a missing action.
set(clipp_shortcuts_actions "${clipp_shortcuts_output}/Metadata.appintents/extract.actionsdata")
if (NOT EXISTS "${clipp_shortcuts_actions}")
    message(FATAL_ERROR "App Intents extraction produced no actions: ${clipp_shortcuts_actions}")
endif()
