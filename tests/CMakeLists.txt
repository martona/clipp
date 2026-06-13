# clipp unit tests (doctest). Compiles the pure engine translation units directly
# and links only doctest -- no platform code, no sodium/zstd/etc. Added from the
# root CMakeLists only when CLIPP_BUILD_TESTS is ON.
find_package(doctest CONFIG REQUIRED)

add_executable(clipp-tests
    test_main.cpp
    test_hlc.cpp
    test_register_store.cpp
    "${CMAKE_SOURCE_DIR}/src/Hlc.cpp"
    "${CMAKE_SOURCE_DIR}/src/RegisterStore.cpp"
)
target_include_directories(clipp-tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(clipp-tests PRIVATE doctest::doctest)
if (MSVC)
    target_compile_options(clipp-tests PRIVATE /utf-8)
endif()

# One CTest entry runs the whole doctest binary; doctest prints per-case results
# itself and returns non-zero on any failure.
add_test(NAME clipp-tests COMMAND clipp-tests)
