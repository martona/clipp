#!/bin/bash
set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

IDENTITY="${APPLE_CODESIGN_IDENTITY:-}"
TEAM_ID="${APPLE_TEAM_ID:-}"
CONFIG="${CLIPP_BUILD_CONFIGURATION:-Release}"

if [[ -z "${CLIPP_CACHE_DIR:-}" ]]; then
    if [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        CLIPP_CACHE_DIR="$XDG_CACHE_HOME/clipp"
    else
        CLIPP_CACHE_DIR="$HOME/Library/Caches/clipp"
    fi
fi

VCPKG_ROOT="${VCPKG_ROOT:-$CLIPP_CACHE_DIR/vcpkg}"
VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-$CLIPP_CACHE_DIR/vcpkg-binary-cache}"
VCPKG_INSTALLED_DIR="${VCPKG_INSTALLED_DIR:-$CLIPP_CACHE_DIR/vcpkg-installed}"

export VCPKG_ROOT
export VCPKG_DEFAULT_BINARY_CACHE

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

echo "[*] Using cache directory: $CLIPP_CACHE_DIR"
echo "[*] Using vcpkg root: $VCPKG_ROOT"
echo "[*] Using vcpkg binary cache: $VCPKG_DEFAULT_BINARY_CACHE"
echo "[*] Using vcpkg installed directory: $VCPKG_INSTALLED_DIR"

mkdir -p "$(dirname "$VCPKG_ROOT")" "$VCPKG_DEFAULT_BINARY_CACHE" "$VCPKG_INSTALLED_DIR"

TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
if [[ -f "$TOOLCHAIN_FILE" ]]; then
    echo "[*] vcpkg repository found at $VCPKG_ROOT."
else
    if [[ ! -e "$VCPKG_ROOT" || -z "$(ls -A "$VCPKG_ROOT" 2>/dev/null)" ]]; then
        echo "[*] vcpkg repository not found at $VCPKG_ROOT. Cloning..."
        git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
    else
        echo "[!] Fatal: $VCPKG_ROOT exists but does not look like a vcpkg repository."
        echo "    Set VCPKG_ROOT to a valid vcpkg checkout or remove the directory and retry."
        exit 1
    fi
fi

[[ -f "$TOOLCHAIN_FILE" ]] || {
    echo "[!] Fatal: Could not locate vcpkg.cmake at expected path: $TOOLCHAIN_FILE"
    exit 1
}

if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
    echo "[*] Bootstrapping vcpkg..."
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

CMAKE_ARGS=(
    -DVCPKG_MANIFEST_DIR="$REPO_ROOT/src"
    -DVCPKG_INSTALLED_DIR="$VCPKG_INSTALLED_DIR"
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
