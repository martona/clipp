#!/bin/bash
set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

IDENTITY="${APPLE_CODESIGN_IDENTITY:-}"
TEAM_ID="${APPLE_TEAM_ID:-}"
CONFIG="Release"
VERSION=""
clean=0
notarize=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--debug | --release] [--clean] [--notarize] [--version W.X.Y.Z]

Builds the macOS app.

Options:
  --debug          Build the Debug configuration.
  --release        Build the Release configuration (default).
  --clean          Remove the build directory before building.
  --version VER    Stamp the binary and bundle with this version (W.X.Y.Z).
                   Omit to let CMake default to 0.0.0.0 (local dev builds).
  --notarize       After signing, submit to Apple's notary service and staple.
                   Requires APPLE_CODESIGN_IDENTITY to be a Developer ID Application
                   cert and the App Store Connect API credentials in env:
                     APPLE_API_KEY_PATH   (path to AuthKey_XXXX.p8)
                     APPLE_API_KEY_ID     (10-char key id)
                     APPLE_API_ISSUER_ID  (team issuer UUID)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) CONFIG="Debug"; shift ;;
        --release) CONFIG="Release"; shift ;;
        --clean) clean=1; shift ;;
        --notarize) notarize=1; shift ;;
        --version)
            if [[ -z "${2:-}" ]]; then
                echo "[!] --version requires a value (e.g. --version 1.2.3.4)" >&2
                exit 2
            fi
            VERSION="$2"
            shift 2
            ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[!] Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# Fail fast on misconfigured --notarize so we don't burn a build before noticing.
if [[ "$notarize" == "1" ]]; then
    if [[ -z "$IDENTITY" ]]; then
        echo "[!] Fatal: --notarize requires APPLE_CODESIGN_IDENTITY to be set." >&2
        exit 2
    fi
    : "${APPLE_API_KEY_PATH:?--notarize requires APPLE_API_KEY_PATH (path to .p8)}"
    : "${APPLE_API_KEY_ID:?--notarize requires APPLE_API_KEY_ID}"
    : "${APPLE_API_ISSUER_ID:?--notarize requires APPLE_API_ISSUER_ID}"
    if [[ ! -f "$APPLE_API_KEY_PATH" ]]; then
        echo "[!] Fatal: APPLE_API_KEY_PATH does not point at a readable file: $APPLE_API_KEY_PATH" >&2
        exit 2
    fi
fi

CLIPP_CACHE_DIR="${CLIPP_CACHE_DIR:-$HOME/Library/Caches/clipp}"
VCPKG_ROOT="${VCPKG_ROOT:-$CLIPP_CACHE_DIR/vcpkg}"
VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-$CLIPP_CACHE_DIR/vcpkg-binary-cache}"
VCPKG_INSTALLED_DIR="$CLIPP_CACHE_DIR/vcpkg-installed"

export VCPKG_ROOT
export VCPKG_DEFAULT_BINARY_CACHE

DEV_DIR="$(xcode-select -p 2>/dev/null)" || {
    echo "[!] Fatal: Xcode Command Line Tools are not installed. Run: xcode-select --install"
    exit 1
}

if [[ "$DEV_DIR" == */Xcode.app/Contents/Developer ]]; then
    USE_XCODE=1
    BUILD_DIR="build"
    APP_PATH="$BUILD_DIR/$CONFIG/Clipp.app"
else
    USE_XCODE=0
    BUILD_DIR="build"
    APP_PATH="$BUILD_DIR/Clipp.app"
fi

if [[ "$clean" == "1" ]]; then
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
    -DVCPKG_INSTALL_OPTIONS="--clean-buildtrees-after-build;--clean-packages-after-build"
)

if [[ -n "$VERSION" ]]; then
    CMAKE_ARGS+=(-DCLIPP_VERSION="$VERSION")
fi

if [[ "$USE_XCODE" == "1" ]]; then
    echo "[*] Generating Xcode build files..."

    SIGN_ARGS=(-DCLIPP_MACOS_ENABLE_CODE_SIGNING=OFF)
    if [[ -n "$IDENTITY" ]]; then
        SIGN_ARGS=(
            -DCLIPP_MACOS_ENABLE_CODE_SIGNING=ON
            -DCLIPP_MACOS_CODE_SIGN_IDENTITY="$IDENTITY"
            -DCLIPP_MACOS_DEVELOPMENT_TEAM="$TEAM_ID"
            -DCLIPP_MACOS_ENABLE_HARDENED_RUNTIME=ON
        )
    fi

    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Xcode "${CMAKE_ARGS[@]}" "${SIGN_ARGS[@]}"
    echo "[*] Building Clipp with Xcode ($CONFIG)..."
    cmake --build "$BUILD_DIR" --config "$CONFIG"
else
    echo "[*] Generating command-line build files..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" "${CMAKE_ARGS[@]}"

    echo "[*] Building Clipp..."
    cmake --build "$BUILD_DIR"

    if [[ -n "$IDENTITY" ]]; then
        echo "[*] Signing Clipp with identity from APPLE_CODESIGN_IDENTITY..."
        codesign --force --deep --options=runtime --timestamp \
            --entitlements "$REPO_ROOT/src/platform/macos/Clipp.entitlements" \
            --sign "$IDENTITY" "$APP_PATH"
    fi
fi

if [[ -n "$IDENTITY" ]]; then
    echo "[*] Verifying signature..."
    codesign --verify --deep --strict --verbose=2 "$APP_PATH"

    # Confirm the hardened-runtime flag actually landed in the signature; cheaper to
    # fail here than 5 minutes into a notarytool round trip.
    if ! codesign --display --verbose=4 "$APP_PATH" 2>&1 | grep -Eq 'flags=.*runtime'; then
        if [[ "$notarize" == "1" ]]; then
            echo "[!] Fatal: hardened runtime flag not present on $APP_PATH. Notarization would be rejected." >&2
            exit 1
        else
            echo "[!] Warning: hardened runtime flag not present on $APP_PATH. Notarization will be rejected." >&2
        fi
    fi

    ZIP_PATH="$BUILD_DIR/Clipp.zip"
    echo "[*] Packaging signed app for notarization: $ZIP_PATH"
    # ditto (not zip) preserves macOS metadata correctly; --sequesterRsrc keeps
    # the archive shape notarytool expects; --keepParent puts Clipp.app at the
    # archive root rather than its contents.
    rm -f "$ZIP_PATH"
    /usr/bin/ditto -c -k --sequesterRsrc --keepParent "$APP_PATH" "$ZIP_PATH"
    echo "[*] Notarization-ready archive: $ZIP_PATH"
else
    echo "[*] Skipping codesign. Set APPLE_CODESIGN_IDENTITY to sign the app."
fi

if [[ "$notarize" == "1" ]]; then
    echo "[*] Submitting $ZIP_PATH to Apple's notary service (this can take a few minutes)..."
    # Capture output so we can sanity-check the verdict; --wait blocks until Apple is done.
    NOTARY_LOG="$BUILD_DIR/notarytool-submit.log"
    set +e
    xcrun notarytool submit "$ZIP_PATH" \
        --key "$APPLE_API_KEY_PATH" \
        --key-id "$APPLE_API_KEY_ID" \
        --issuer "$APPLE_API_ISSUER_ID" \
        --wait 2>&1 | tee "$NOTARY_LOG"
    NOTARY_STATUS=${PIPESTATUS[0]}
    set -e

    SUBMISSION_ID="$(grep -E '^[[:space:]]*id:' "$NOTARY_LOG" | head -n1 | awk '{print $2}')"

    # notarytool's exit code reflects upload+protocol success, not the verdict.
    # The verdict is in the final "status:" line — Accepted is the only good one.
    if [[ "$NOTARY_STATUS" -ne 0 ]] || ! grep -Eq '^[[:space:]]*status: Accepted[[:space:]]*$' "$NOTARY_LOG"; then
        echo "[!] Notarization did not return Accepted." >&2
        if [[ -n "$SUBMISSION_ID" ]]; then
            echo "[!] Fetching notary log for submission $SUBMISSION_ID..." >&2
            xcrun notarytool log "$SUBMISSION_ID" \
                --key "$APPLE_API_KEY_PATH" \
                --key-id "$APPLE_API_KEY_ID" \
                --issuer "$APPLE_API_ISSUER_ID" >&2 || true
        fi
        exit 1
    fi

    echo "[*] Notarization accepted. Stapling ticket to $APP_PATH..."
    xcrun stapler staple "$APP_PATH"
    xcrun stapler validate "$APP_PATH"

    echo "[*] Gatekeeper assessment:"
    spctl --assess --type execute --verbose "$APP_PATH"

    # The pre-staple zip is now stale (the .app inside it lacks the ticket).
    # Re-pack so the on-disk archive matches the stapled bundle for distribution.
    echo "[*] Re-packing stapled bundle: $ZIP_PATH"
    rm -f "$ZIP_PATH"
    /usr/bin/ditto -c -k --sequesterRsrc --keepParent "$APP_PATH" "$ZIP_PATH"
fi

echo "[*] Build complete: $APP_PATH"
