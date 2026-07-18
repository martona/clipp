#!/bin/bash
set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# Mac App Store distribution uses a different cert than Developer ID. The
# 3rd-party env var is what build_macos.sh uses for its (non-MAS) Developer ID
# signing; this script reads the MAS variant via APPLE_CODESIGN_IDENTITY_3RDPARTY
# ("3rd Party Mac Developer Application: ..."). Installer cert is separate again.
IDENTITY="${APPLE_CODESIGN_IDENTITY_3RDPARTY:-}"
INSTALLER_IDENTITY="${APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY:-}"
TEAM_ID="${APPLE_TEAM_ID:-}"
PROVISION_PROFILE="${APPLE_MAS_CLIPP_PROVISIONING_PROFILE:-}"
CONFIG="Release"
VERSION=""
clean=0
sign_for_distribution=0
package=0
upload=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--debug | --release] [--clean] [--sign] [--package] [--upload] [--version W.X.Y.Z]

Builds Clipp.app sandboxed for the Mac App Store.

Default (no flags, no env vars): build, then ad-hoc sign with the sandbox
entitlements. Result runs locally and exercises the sandbox; not distributable.
Useful for verifying nothing breaks under sandbox before chasing certs.

Options:
  --debug          Build the Debug configuration.
  --release        Build the Release configuration (default).
  --clean          Remove the build directory before building.
  --version VER    Stamp the binary and bundle with this version (W.X.Y.Z).
                   Omit to let CMake default (local dev builds).
  --sign           Sign for App Store distribution. Requires:
                     APPLE_CODESIGN_IDENTITY_3RDPARTY (3rd Party Mac Developer Application cert)
                     APPLE_MAS_CLIPP_PROVISIONING_PROFILE   (path to embedded.provisionprofile)
  --package        Wrap signed app in a .pkg via productbuild. Implies --sign.
                   Also requires:
                     APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY (3rd Party Mac Developer Installer cert)
  --upload         Submit the .pkg to App Store Connect via xcrun altool. Implies --package.
                   Also requires:
                     APPLE_API_KEY_PATH    (path to AuthKey_XXXX.p8)
                     APPLE_API_KEY_ID      (10-char key id)
                     APPLE_API_ISSUER_ID   (issuer UUID)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) CONFIG="Debug"; shift ;;
        --release) CONFIG="Release"; shift ;;
        --clean) clean=1; shift ;;
        --sign) sign_for_distribution=1; shift ;;
        --package) package=1; sign_for_distribution=1; shift ;;
        --upload) upload=1; package=1; sign_for_distribution=1; shift ;;
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

# Fail fast on missing prerequisites before burning a 10-minute vcpkg+cmake cycle.
if [[ "$sign_for_distribution" == "1" ]]; then
    if [[ -z "$IDENTITY" ]]; then
        echo "[!] Fatal: --sign/--package/--upload require APPLE_CODESIGN_IDENTITY_3RDPARTY ('3rd Party Mac Developer Application' cert)." >&2
        exit 2
    fi
    if [[ -z "$TEAM_ID" ]]; then
        echo "[!] Fatal: --sign/--package/--upload require APPLE_TEAM_ID (your 10-char Apple Team ID, e.g. 2262A4CP8N)." >&2
        exit 2
    fi
    if [[ -z "$PROVISION_PROFILE" ]]; then
        echo "[!] Fatal: --sign/--package/--upload require APPLE_MAS_CLIPP_PROVISIONING_PROFILE (path to embedded.provisionprofile)." >&2
        exit 2
    fi
    if [[ ! -f "$PROVISION_PROFILE" ]]; then
        echo "[!] Fatal: APPLE_MAS_CLIPP_PROVISIONING_PROFILE does not point at a file: $PROVISION_PROFILE" >&2
        exit 2
    fi
fi
if [[ "$package" == "1" ]]; then
    if [[ -z "$INSTALLER_IDENTITY" ]]; then
        echo "[!] Fatal: --package/--upload require APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY ('3rd Party Mac Developer Installer' cert)." >&2
        exit 2
    fi
    # The version is tag-canonical (no in-tree stamp); a .pkg is a store
    # artifact, so without --version it would ship as 0.0.0.0.
    if [[ -z "$VERSION" ]]; then
        echo "[!] Fatal: --package/--upload require --version W.X.Y.Z (should match the release tag)." >&2
        exit 2
    fi
fi
if [[ "$upload" == "1" ]]; then
    : "${APPLE_API_KEY_PATH:?--upload requires APPLE_API_KEY_PATH (path to .p8)}"
    : "${APPLE_API_KEY_ID:?--upload requires APPLE_API_KEY_ID}"
    : "${APPLE_API_ISSUER_ID:?--upload requires APPLE_API_ISSUER_ID}"
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

# Separate build dir from build_macos.sh so the two flows can coexist without
# cmake reconfigure thrash when switching between Developer ID and MAS.
if [[ "$DEV_DIR" == */Xcode.app/Contents/Developer ]]; then
    USE_XCODE=1
    BUILD_DIR="build-mas"
    APP_PATH="$BUILD_DIR/$CONFIG/Clipp.app"
else
    USE_XCODE=0
    BUILD_DIR="build-mas"
    APP_PATH="$BUILD_DIR/Clipp.app"
fi

ENTITLEMENTS_FILE="$REPO_ROOT/src/platform/macos/Clipp.mas.entitlements"

if [[ ! -f "$ENTITLEMENTS_FILE" ]]; then
    echo "[!] Fatal: MAS entitlements file missing: $ENTITLEMENTS_FILE" >&2
    exit 1
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

# Always build unsigned; sign post-build so we can swap entitlements/identity
# without round-tripping through cmake reconfigure. Avoids Xcode's auto-signing
# heuristics interfering with the MAS-specific provisioning profile embedding.
SIGN_ARGS=(-DCLIPP_MACOS_ENABLE_CODE_SIGNING=OFF)

if [[ "$USE_XCODE" == "1" ]]; then
    echo "[*] Generating Xcode build files..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Xcode "${CMAKE_ARGS[@]}" "${SIGN_ARGS[@]}"
    echo "[*] Building Clipp ($CONFIG)..."
    cmake --build "$BUILD_DIR" --config "$CONFIG"
else
    echo "[*] Generating command-line build files..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" "${CMAKE_ARGS[@]}" "${SIGN_ARGS[@]}"
    echo "[*] Building Clipp..."
    cmake --build "$BUILD_DIR"
fi

if [[ ! -d "$APP_PATH" ]]; then
    echo "[!] Fatal: Expected app bundle not found at $APP_PATH" >&2
    exit 1
fi

# The build tree may live on a network share (SMB from a Windows host in this
# setup). SMB doesn't honor POSIX chmod and silently drops in-place plist edits, so
# post-build surgery done directly on the share doesn't survive into the pkg (this
# is why the CFBundleVersion rewrite, world-readable perms, and signed entitlements
# all reverted on earlier attempts). When signing for distribution, copy the freshly
# built bundle onto local disk and do everything there, where filesystem semantics
# are normal. The ad-hoc local-test path stays in place — it just needs to run.
if [[ "$sign_for_distribution" == "1" ]]; then
    STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/clipp-mas.XXXXXX")"
    trap 'rm -rf "$STAGE_DIR"' EXIT
    echo "[*] Staging app bundle to local disk (build tree may be on a network share): $STAGE_DIR"
    ditto "$APP_PATH" "$STAGE_DIR/Clipp.app"
    APP_PATH="$STAGE_DIR/Clipp.app"
fi

INFO_PLIST="$APP_PATH/Contents/Info.plist"

# MAS rejects a 4-part CFBundleVersion (error 90257: "at most three non-negative
# integers"). Our W.X.Y.Z scheme carries the monotonic build counter in the 4th
# component, so collapse CFBundleVersion down to just that integer -- unique per
# upload and always increasing. CFBundleShortVersionString (user-visible W.X.Y)
# is left untouched.
FULL_BUNDLE_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$INFO_PLIST")"
MAS_BUNDLE_VERSION="${FULL_BUNDLE_VERSION##*.}"
if [[ "$MAS_BUNDLE_VERSION" != "$FULL_BUNDLE_VERSION" ]]; then
    echo "[*] Rewriting CFBundleVersion $FULL_BUNDLE_VERSION -> $MAS_BUNDLE_VERSION for MAS (<=3 integers)..."
    /usr/libexec/PlistBuddy -c "Set :CFBundleVersion $MAS_BUNDLE_VERSION" "$INFO_PLIST"
fi
echo "[*] CFBundleVersion in bundle is now: $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$INFO_PLIST")"

if [[ "$sign_for_distribution" == "1" ]]; then
    echo "[*] Embedding provisioning profile from $PROVISION_PROFILE..."
    cp "$PROVISION_PROFILE" "$APP_PATH/Contents/embedded.provisionprofile"
fi

# codesign rejects a bundle carrying any extended-attribute "detritus" (quarantine
# flags, Finder info, resource forks). The most common source here is
# com.apple.quarantine on the just-copied provisioning profile — it was downloaded
# via a browser, which tags it. Strip xattrs across the bundle before signing;
# this only removes metadata, not file content, so the profile stays valid.
xattr -cr "$APP_PATH"

# productbuild preserves on-disk POSIX permissions. If any file in the bundle is
# not world-readable, App Store Connect rejects the pkg (error 90255, reported as
# "files only readable by the root user"). On local disk (post-staging) this
# actually sticks. +X adds execute only where it already exists (dirs, the binary).
chmod -R a+rX "$APP_PATH"

if [[ "$sign_for_distribution" == "1" ]]; then
    # App Store signatures must embed com.apple.application-identifier
    # (AppIDPrefix.BundleID) and com.apple.developer.team-identifier. Xcode injects
    # these from the provisioning profile automatically; since we sign manually with
    # a static entitlements file, we add them ourselves. Without application-identifier
    # the signature mismatches the embedded profile and ASC flags the build ineligible
    # (warning 90886 / not valid for TestFlight).
    BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$INFO_PLIST")"

    # application-identifier must match the profile byte-for-byte. Its prefix is the
    # App ID Prefix -- usually the Team ID, occasionally a legacy seed ID -- so read
    # the authoritative value out of the profile itself. Profiles store the key with
    # or without the com.apple. prefix depending on vintage; try both, then fall back
    # to TeamID.BundleID.
    PROFILE_DECODED="$STAGE_DIR/embedded.profile.plist"
    security cms -D -i "$PROVISION_PROFILE" -o "$PROFILE_DECODED" 2>/dev/null || true
    APP_IDENTIFIER="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.application-identifier' "$PROFILE_DECODED" 2>/dev/null || true)"
    if [[ -z "$APP_IDENTIFIER" ]]; then
        APP_IDENTIFIER="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:application-identifier' "$PROFILE_DECODED" 2>/dev/null || true)"
    fi
    if [[ -z "$APP_IDENTIFIER" ]]; then
        APP_IDENTIFIER="${TEAM_ID}.${BUNDLE_ID}"
    fi

    EFFECTIVE_ENTITLEMENTS="$STAGE_DIR/Clipp.effective.entitlements"
    cp "$ENTITLEMENTS_FILE" "$EFFECTIVE_ENTITLEMENTS"
    /usr/libexec/PlistBuddy -c "Add :com.apple.application-identifier string ${APP_IDENTIFIER}" "$EFFECTIVE_ENTITLEMENTS"
    /usr/libexec/PlistBuddy -c "Add :com.apple.developer.team-identifier string ${TEAM_ID}" "$EFFECTIVE_ENTITLEMENTS"

    echo "[*] Signing Clipp.app with Apple Distribution identity ($IDENTITY)..."
    echo "    application-identifier: $APP_IDENTIFIER"
    codesign --force --deep --options=runtime --timestamp \
        --entitlements "$EFFECTIVE_ENTITLEMENTS" \
        --sign "$IDENTITY" "$APP_PATH"
else
    # Ad-hoc sign with the sandbox entitlements. macOS only enforces the sandbox
    # against an entitlement-bearing *signed* bundle; without this step the
    # entitlements file would be inert and the app would run unrestricted,
    # defeating the point of a "test the sandbox" build.
    echo "[*] Ad-hoc signing for local sandbox testing (no distribution identity used)..."
    codesign --force --deep --options=runtime \
        --entitlements "$ENTITLEMENTS_FILE" \
        --sign - "$APP_PATH"
fi

echo "[*] Verifying signature..."
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

# Confirm the sandbox entitlement actually landed in the signed bundle. Cheaper
# to fail here than to find out at App Store Connect upload time.
if ! codesign --display --entitlements :- "$APP_PATH" 2>/dev/null | grep -q "com.apple.security.app-sandbox"; then
    echo "[!] Fatal: app-sandbox entitlement missing from signed bundle." >&2
    exit 1
fi

if [[ "$sign_for_distribution" == "1" ]]; then
    # application-identifier must be present (and match the embedded profile) or the
    # upload is flagged ineligible (90886). Catch it here, not at upload time.
    if ! codesign --display --entitlements :- "$APP_PATH" 2>/dev/null | grep -q "com.apple.application-identifier"; then
        echo "[!] Fatal: com.apple.application-identifier missing from signed bundle." >&2
        exit 1
    fi
fi

if [[ "$sign_for_distribution" == "1" ]]; then
    # Hardened-runtime flag check mirrors build_macos.sh — MAS doesn't strictly
    # require it but Apple's review pipeline complains if it's missing.
    if ! codesign --display --verbose=4 "$APP_PATH" 2>&1 | grep -Eq 'flags=.*runtime'; then
        echo "[!] Warning: hardened runtime flag not present on $APP_PATH." >&2
    fi
fi

if [[ "$package" == "1" ]]; then
    PKG_PATH="$BUILD_DIR/Clipp.pkg"
    echo "[*] Wrapping signed app in .pkg via productbuild: $PKG_PATH"
    rm -f "$PKG_PATH"
    productbuild --component "$APP_PATH" /Applications \
        --sign "$INSTALLER_IDENTITY" \
        "$PKG_PATH"

    echo "[*] Verifying .pkg signature..."
    pkgutil --check-signature "$PKG_PATH"
fi

if [[ "$upload" == "1" ]]; then
    # altool's --apiKey expects the 10-char key id and looks for the matching
    # .p8 file in ~/.appstoreconnect/private_keys/AuthKey_<id>.p8. Stage the
    # user-supplied .p8 path into that location if it isn't already there.
    KEYS_DIR="$HOME/.appstoreconnect/private_keys"
    EXPECTED_KEY="$KEYS_DIR/AuthKey_${APPLE_API_KEY_ID}.p8"
    if [[ ! -f "$EXPECTED_KEY" ]]; then
        mkdir -p "$KEYS_DIR"
        cp "$APPLE_API_KEY_PATH" "$EXPECTED_KEY"
        echo "[*] Staged API key at $EXPECTED_KEY"
    fi

    UPLOAD_LOG="$BUILD_DIR/altool-upload.log"
    echo "[*] Uploading $PKG_PATH to App Store Connect (may take several minutes)..."
    set +e
    xcrun altool --upload-app -f "$PKG_PATH" -t macos \
        --apiKey "$APPLE_API_KEY_ID" \
        --apiIssuer "$APPLE_API_ISSUER_ID" 2>&1 | tee "$UPLOAD_LOG"
    UPLOAD_STATUS=${PIPESTATUS[0]}
    set -e

    if [[ "$UPLOAD_STATUS" -ne 0 ]]; then
        echo "[!] Fatal: altool upload returned status $UPLOAD_STATUS. See $UPLOAD_LOG." >&2
        exit 1
    fi
    echo "[*] Upload complete. Check App Store Connect for processing status."
fi

echo "[*] Build complete: $APP_PATH"
