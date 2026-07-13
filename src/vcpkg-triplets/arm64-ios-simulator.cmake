set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_SYSROOT iphonesimulator)
# See arm64-ios.cmake for the rationale. Keep both triplets in sync.
set(VCPKG_OSX_DEPLOYMENT_TARGET 16.0)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-apple-ios-simulator")
