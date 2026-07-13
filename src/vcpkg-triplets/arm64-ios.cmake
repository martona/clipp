set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_SYSROOT iphoneos)
# Despite the OSX_ prefix, this maps to CMAKE_OSX_DEPLOYMENT_TARGET — when
# CMAKE_SYSTEM_NAME=iOS, that's the iOS deployment target. Must match (or be
# lower than) the Xcode project's IPHONEOS_DEPLOYMENT_TARGET, or the linker
# refuses to consume the vcpkg-built static libs.
set(VCPKG_OSX_DEPLOYMENT_TARGET 16.0)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-apple-ios")
