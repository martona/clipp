#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

CONFIG="Release"
PROJECT="ios/Clipp.xcodeproj"
TARGET="Clipp"
SDK="iphonesimulator"
BUILD_DIR="$REPO_ROOT/build/ios"
SYMROOT="$BUILD_DIR/Build"
OBJROOT="$BUILD_DIR/Intermediates"

DISABLE_CODE_SIGNING=0
SETUP_VCPKG=1
clean=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--debug | --release] [--disable-code-signing] [--skip-vcpkg] [--clean]

Builds the iOS app for the simulator.

Options:
  --debug                 Build the Debug configuration.
  --release               Build the Release configuration (default).
  --disable-code-signing  Pass CODE_SIGNING_ALLOWED=NO to xcodebuild.
  --skip-vcpkg            Do not run setup_ios_vcpkg.sh first.
  --clean                 Remove the iOS build directory before building.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)
            CONFIG="Debug"
            shift
            ;;
        --release)
            CONFIG="Release"
            shift
            ;;
        --disable-code-signing)
            DISABLE_CODE_SIGNING=1
            shift
            ;;
        --skip-vcpkg)
            SETUP_VCPKG=0
            shift
            ;;
        --clean)
            clean=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[!] Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

need_tool() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "[!] Fatal: $1 is required but was not found in PATH." >&2
        exit 1
    }
}

need_tool xcodebuild
need_tool xcrun

xcrun --sdk "$SDK" --show-sdk-path >/dev/null

if [[ "$clean" == "1" ]]; then
    echo "[*] Cleaning iOS build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

if [[ "$SETUP_VCPKG" == "1" ]]; then
    echo "[*] Setting up iOS simulator vcpkg dependencies..."
    "$SCRIPT_DIR/setup_ios_vcpkg.sh" --simulator-only
fi

mkdir -p "$BUILD_DIR"

xcodebuild_args=(
    -project "$PROJECT"
    -target "$TARGET"
    -configuration "$CONFIG"
    -sdk "$SDK"
    SYMROOT="$SYMROOT"
    OBJROOT="$OBJROOT"
)

if [[ "$DISABLE_CODE_SIGNING" == "1" ]]; then
    xcodebuild_args+=(CODE_SIGNING_ALLOWED=NO)
fi

echo "[*] Building iOS simulator app ($CONFIG)..."
xcodebuild "${xcodebuild_args[@]}" build

APP_PATH="$SYMROOT/$CONFIG-iphonesimulator/$TARGET.app"
if [[ -d "$APP_PATH" ]]; then
    echo "[*] Build complete: $APP_PATH"
else
    echo "[*] Build complete. Expected app path not found; searching under $BUILD_DIR..."
    find "$BUILD_DIR" -name "$TARGET.app" -type d -print | sed -n '1,20p'
fi
