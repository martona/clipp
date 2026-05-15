#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

BUILD_DIR="${BUILD_DIR:-build}"
APP_PATH="$BUILD_DIR/clipp.app"
CODESIGN_IDENTITY="${CLIPP_CODESIGN_IDENTITY:-${APPLE_CODESIGN_IDENTITY:-}}"

# Parse command line arguments
if [[ "$1" == "--clean" ]]; then
    echo "[*] Clean flag detected. Nuking build directory..."
    rm -rf "$BUILD_DIR"
fi

echo "[*] Checking local dependencies..."

# 1. Ensure Xcode Command Line Tools are installed
if ! xcode-select -p &> /dev/null; then
    echo "[!] Fatal: Xcode Command Line Tools are not installed. Run: xcode-select --install"
    exit 1
fi

# 2. Ensure Homebrew-backed tools are installed
if ! command -v cmake &> /dev/null; then
    if ! command -v brew &> /dev/null; then
        echo "[!] Fatal: CMake is not installed and Homebrew is unavailable. Install CMake and retry."
        exit 1
    fi
    echo "[*] CMake not found. Installing via Homebrew..."
    brew install cmake
fi

if ! command -v ninja &> /dev/null; then
    if ! command -v brew &> /dev/null; then
        echo "[!] Fatal: Ninja is not installed and Homebrew is unavailable. Install Ninja and retry."
        exit 1
    fi
    echo "[*] Ninja not found. Installing via Homebrew..."
    brew install ninja
fi

# 3. Setup vcpkg via the official Git method
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

echo "[*] Generating command-line build files..."

# Run CMake to generate and build using the Xcode Command Line Tools toolchain.
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DVCPKG_MANIFEST_DIR="$(pwd)/src" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"

echo "[*] Building Clipp..."
cmake --build "$BUILD_DIR"

if [[ -n "$CODESIGN_IDENTITY" ]]; then
    if ! command -v codesign &> /dev/null; then
        echo "[!] Fatal: codesign is not available. Install the Xcode Command Line Tools and retry."
        exit 1
    fi

    if [ ! -d "$APP_PATH" ]; then
        echo "[!] Fatal: app bundle was not found at $APP_PATH"
        exit 1
    fi

    echo "[*] Signing Clipp with identity from CLIPP_CODESIGN_IDENTITY..."
    codesign --force --deep --sign "$CODESIGN_IDENTITY" "$APP_PATH"

    echo "[*] Verifying signature..."
    codesign --verify --deep --strict --verbose=2 "$APP_PATH"
else
    echo "[*] Skipping codesign. Set CLIPP_CODESIGN_IDENTITY to sign the app."
fi

echo "[*] Build complete: $APP_PATH"
