#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# Parse command line arguments
if [[ "$1" == "--clean" ]]; then
    echo "[*] Clean flag detected. Nuking build directory..."
    rm -rf build/
fi

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

# 3. Setup vcpkg via the official Git method instead
VCPKG_ROOT="$HOME/vcpkg"

if [ ! -d "$VCPKG_ROOT" ]; then
    echo "[*] vcpkg repository not found at $VCPKG_ROOT. Cloning..."
    git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
    
    echo "[*] Bootstrapping vcpkg..."
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
else
    echo "[*] vcpkg repository found at $VCPKG_ROOT."
fi

# 4. Locate the toolchain file inside the cloned repo
TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

if [ ! -f "$TOOLCHAIN_FILE" ]; then
    echo "[!] Fatal: Could not locate vcpkg.cmake at expected path: $TOOLCHAIN_FILE"
    exit 1
fi

echo "[*] Generating Xcode project..."

# Run CMake to generate
cmake -B build -G Xcode -DVCPKG_MANIFEST_DIR="$(pwd)/src" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"

echo "[*] Build environment ready."
echo "[*] Launching Xcode..."

# Open Xcode
# open build/clipp.xcodeproj
