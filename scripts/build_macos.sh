#!/bin/bash
set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

IDENTITY="${APPLE_CODESIGN_IDENTITY:-}"
TEAM_ID="${APPLE_TEAM_ID:-}"
CONFIG="${CLIPP_BUILD_CONFIGURATION:-Release}"

DEV_DIR="$(xcode-select -p 2>/dev/null)" || {
    echo "[!] Fatal: Xcode Command Line Tools are not installed. Run: xcode-select --install"
    exit 1
}

if [[ "$DEV_DIR" == */Xcode.app/Contents/Developer ]]; then
    USE_XCODE=1
    BUILD_DIR="${BUILD_DIR:-build}"
    APP_PATH="$BUILD_DIR/$CONFIG/clipp.app"
else
    USE_XCODE=0
    BUILD_DIR="${BUILD_DIR:-build}"
    APP_PATH="$BUILD_DIR/clipp.app"
fi

if [[ "${1:-}" == "--clean" ]]; then
    echo "[*] Clean flag detected. Nuking build directory..."
    rm -rf "$BUILD_DIR"
fi

need_tool() {
    command -v "$1" &> /dev/null && return
    command -v brew &> /dev/null || { echo "[!] Fatal: $1 is not installed and Homebrew is unavailable."; exit 1; }
    echo "[*] $1 not found. Installing via Homebrew..."
    brew install "$1"
}

echo "[*] Checking local dependencies..."
need_tool cmake
[[ "$USE_XCODE" == "1" ]] || need_tool ninja

VCPKG_ROOT="$HOME/vcpkg"
if [[ -d "$VCPKG_ROOT" ]]; then
    echo "[*] vcpkg repository found at $VCPKG_ROOT."
else
    echo "[*] vcpkg repository not found at $VCPKG_ROOT. Cloning..."
    git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
[[ -f "$TOOLCHAIN_FILE" ]] || {
    echo "[!] Fatal: Could not locate vcpkg.cmake at expected path: $TOOLCHAIN_FILE"
    exit 1
}

CMAKE_ARGS=(
    -DVCPKG_MANIFEST_DIR="$REPO_ROOT/src"
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
)

if [[ "$USE_XCODE" == "1" ]]; then
    echo "[*] Generating Xcode build files..."

    SIGN_ARGS=(-DCLIPP_MACOS_ENABLE_CODE_SIGNING=OFF)
    if [[ -n "$IDENTITY" ]]; then
        SIGN_ARGS=(
            -DCLIPP_MACOS_ENABLE_CODE_SIGNING=ON
            -DCLIPP_MACOS_CODE_SIGN_IDENTITY="$IDENTITY"
            -DCLIPP_MACOS_DEVELOPMENT_TEAM="$TEAM_ID"
        )
    fi

    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Xcode "${CMAKE_ARGS[@]}" "${SIGN_ARGS[@]}"
    echo "[*] Building Clipp with Xcode ($CONFIG)..."
    cmake --build "$BUILD_DIR" --config "$CONFIG"
else
    echo "[*] Generating command-line build files..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja "${CMAKE_ARGS[@]}"

    echo "[*] Building Clipp..."
    cmake --build "$BUILD_DIR"

    if [[ -n "$IDENTITY" ]]; then
        echo "[*] Signing Clipp with identity from APPLE_CODESIGN_IDENTITY..."
        codesign --force --deep --sign "$IDENTITY" "$APP_PATH"
    fi
fi

if [[ -n "$IDENTITY" ]]; then
    echo "[*] Verifying signature..."
    codesign --verify --deep --strict --verbose=2 "$APP_PATH"
else
    echo "[*] Skipping codesign. Set APPLE_CODESIGN_IDENTITY to sign the app."
fi

echo "[*] Build complete: $APP_PATH"
