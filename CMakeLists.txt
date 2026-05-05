cmake_minimum_required(VERSION 3.20)
project(clipp)

set(CMAKE_CXX_STANDARD 20)

file(GLOB SOURCES "src/*.cpp" "src/*.h")
list(FILTER SOURCES EXCLUDE REGEX "vcpkg_installed")

if (APPLE)
    list(FILTER SOURCES EXCLUDE REGEX "platform_win32_*")
endif()

if (WIN32)
    list(FILTER SOURCES EXCLUDE REGEX "platform_mac_*")
endif()

add_executable(clipp ${SOURCES})

if (APPLE)
    target_compile_options(clipp PRIVATE "-Wno-deprecated-declarations")
endif()

find_package(unofficial-sodium CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

#find_package(unofficial-sodium CONFIG REQUIRED)
## Force CMake to print the absolute path where it found the configuration, then crash
#message(FATAL_ERROR ">>> GHOST CACHE LOCATED AT: ${unofficial-sodium_DIR} <<<")

target_link_libraries(clipp PRIVATE 
    unofficial-sodium::sodium
    xxHash::xxhash
    zstd::libzstd
)
