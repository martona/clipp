#!/bin/bash
set -e

echo "[*] Checking local dependencies..."

# 1. Ensure Homebrew is installed
if ! command -v brew &> /dev/null; then
    echo "[!] Fatal: Homebrew is not installed. Please install it from brew.sh first."
    exit 1
fi

# 2. Ensure CMake is installed
if ! command -v cmake &> /dev/null; then
    echo "[*] CMake not found. Installing via Homebrew..."
    brew install cmake
fi

# 3. Ensure vcpkg is installed
if ! command -v vcpkg &> /dev/null; then
    echo "[*] vcpkg not found. Installing via Homebrew..."
    brew install vcpkg
fi

# 4. Locate the vcpkg toolchain file
VCPKG_PREFIX=$(brew --prefix vcpkg)
TOOLCHAIN_FILE="$VCPKG_PREFIX/libexec/scripts/buildsystems/vcpkg.cmake"

if [ ! -f "$TOOLCHAIN_FILE" ]; then
    echo "[!] Fatal: Could not locate vcpkg.cmake at expected path: $TOOLCHAIN_FILE"
    exit 1
fi

echo "[*] Generating Xcode project..."

# Run CMake. 
cmake -B build -G Xcode -DVCPKG_MANIFEST_DIR="src" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"

echo "[*] Build environment ready."
echo "[*] Launching Xcode..."

# Open the generated project in Xcode
open build/clipp.xcodeproj
