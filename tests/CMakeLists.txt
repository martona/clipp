# clipp unit tests (doctest). Compiles the pure engine translation units directly
# and links only doctest -- no platform code. ClipboardActivityStore.cpp (the
# container half; the display half lives in ClipboardActivityDisplay.cpp and is
# NOT built here) reaches the sodium/xxhash/zstd headers via ClipboardPayload.h,
# so those packages are linked for their include paths and any stray inline
# references -- no crypto/compression actually runs in these tests.
# Added from the root CMakeLists only when CLIPP_BUILD_TESTS is ON.
find_package(doctest CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

add_executable(clipp-tests
    test_main.cpp
    test_hlc.cpp
    test_register_store.cpp
    test_register_wire.cpp
    test_binary_registers.cpp
    test_register_persistence.cpp
    test_clipboard_persistence.cpp
    test_clipboard_activity_store.cpp
    test_popup_model.cpp
    # Logger.cpp satisfies g_logger for ClipboardPayload.h's inline over-cap
    # logging, instantiated by the clipboard-persistence tests. Nothing
    # configures a log file here, so lines go nowhere — that's fine.
    "${CMAKE_SOURCE_DIR}/src/Logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/Hlc.cpp"
    "${CMAKE_SOURCE_DIR}/src/RegisterStore.cpp"
    "${CMAKE_SOURCE_DIR}/src/RegisterWire.cpp"
    "${CMAKE_SOURCE_DIR}/src/RegisterPersistence.cpp"
    "${CMAKE_SOURCE_DIR}/src/SealedSnapshot.cpp"
    "${CMAKE_SOURCE_DIR}/src/ClipboardPersistence.cpp"
    "${CMAKE_SOURCE_DIR}/src/ClipboardActivityStore.cpp"
    "${CMAKE_SOURCE_DIR}/src/PopupModel.cpp"
)
target_include_directories(clipp-tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(clipp-tests PRIVATE
    doctest::doctest
    unofficial-sodium::sodium
    xxHash::xxhash
    zstd::libzstd
)
if (WIN32)
    # ClipboardPersistence.cpp round-trips the wire CLIP-frame byte order via
    # NetworkDefs' hton/ntoh helpers — winsock imports on Windows.
    target_link_libraries(clipp-tests PRIVATE ws2_32)
endif()
if (MSVC)
    target_compile_options(clipp-tests PRIVATE /utf-8)
endif()

# One CTest entry runs the whole doctest binary; doctest prints per-case results
# itself and returns non-zero on any failure.
add_test(NAME clipp-tests COMMAND clipp-tests)
