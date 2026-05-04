cmake_minimum_required(VERSION 3.20)
project(clipp)

set(CMAKE_CXX_STANDARD 20)

file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h")

if (APPLE)
    list(FILTER SOURCES EXCLUDE REGEX "platform_win32_*")
endif()

if (WIN32)
    list(FILTER SOURCES EXCLUDE REGEX "platform_mac_*")
endif()

add_executable(clipp ${SOURCES})

find_package(unofficial-sodium CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

target_link_libraries(clipp PRIVATE 
    unofficial-sodium::sodium
    xxHash::xxhash
    zstd::libzstd
)
