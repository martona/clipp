#!/bin/bash
set -euo pipefail

# Builds, archives, signs, and (optionally) uploads the Clipp iOS app -- with
# its share extension -- to App Store Connect. Fully headless: no Xcode GUI,
# no Xcode Apple-ID session. Ported from ../gig, extended for the second
# target (the share extension is its own bundle id and needs its own profile;
# App Groups on both targets rules out a wildcard profile).
#
# Signing model (deliberate, learned the hard way):
#   - The ARCHIVE signs automatically with the local development identity
#     (pbxproj is CODE_SIGN_STYLE=Automatic with the team baked in); the API
#     key only lets xcodebuild refresh dev profiles, which any key role may.
#   - The EXPORT re-signs for distribution MANUALLY: the Apple Distribution
#     identity in the keychain + the two iOS App Store provisioning profiles
#     pointed at by APPLE_IOS_CLIPP_PROVISIONING_PROFILE (app) and
#     APPLE_IOS_CLIPPSE_PROVISIONING_PROFILE (share extension). Cloud signing
#     is NOT used here: minting profiles on the fly needs an Admin-role API
#     key ("Cloud signing permission error"), which we deliberately avoid.
#   - The API key trio authenticates the UPLOAD only (any role works).
#
# Version: clipp's version is tag-canonical (no in-tree stamp); --version
# W.X.Y.Z is REQUIRED and should match the release tag. It is applied at
# archive time (MARKETING_VERSION = W.X.Y, CURRENT_PROJECT_VERSION = W.X.Y.Z),
# which stamps the app and the share extension in lockstep; the 0.0.0(.0)
# placeholders in the pbxproj never ship (asserted post-archive below).

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

PROJECT="ios/Clipp.xcodeproj"
SCHEME="Clipp"
CONFIG="Release"
BUILD_DIR="$REPO_ROOT/build/ios-appstore"
ARCHIVE_PATH="$BUILD_DIR/Clipp.xcarchive"
EXPORT_PATH="$BUILD_DIR/export"
VERSION=""
SETUP_VCPKG=1
clean=0
upload=0

APP_BUNDLE_ID="net.clipp.ios"
APPEX_BUNDLE_ID="net.clipp.ios.ShareExtension"

# App-scoped profile env names (see build_macos_mas.sh for the rationale: a
# shared env name once shipped the wrong app's profile, ITMS-90286).
APP_PROFILE="${APPLE_IOS_CLIPP_PROVISIONING_PROFILE:-}"
APPEX_PROFILE="${APPLE_IOS_CLIPPSE_PROVISIONING_PROFILE:-}"
# The distribution identity to sign with. Override only if your certificate
# predates the unified naming (e.g. "iPhone Distribution: ...").
SIGNING_IDENTITY="${APPLE_CODESIGN_IDENTITY_IOS:-Apple Distribution}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --version W.X.Y.Z [--clean] [--skip-vcpkg] [--upload]

Archives the Clipp iOS app (app + share extension) for the App Store and
exports a signed .ipa (or uploads it with --upload). Always Release.

Requires in env:
  APPLE_IOS_CLIPP_PROVISIONING_PROFILE    (path to the app's iOS App Store .mobileprovision)
  APPLE_IOS_CLIPPSE_PROVISIONING_PROFILE  (path to the share extension's .mobileprovision)
  APPLE_API_KEY_PATH                      (path to AuthKey_XXXX.p8)
  APPLE_API_KEY_ID                        (10-char key id)
  APPLE_API_ISSUER_ID                     (issuer UUID)
Optional:
  APPLE_CODESIGN_IDENTITY_IOS             (default "Apple Distribution")

Also requires an Apple Distribution certificate + private key in the login
keychain (check: security find-identity -v -p codesigning).

Options:
  --version VER    REQUIRED. Stamp this build with VER (W.X.Y.Z, should match
                   the release tag).
  --clean          Remove $BUILD_DIR first.
  --skip-vcpkg     Do not run setup_ios_vcpkg.sh first.
  --upload         Upload to App Store Connect instead of exporting the .ipa.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) clean=1; shift ;;
        --skip-vcpkg) SETUP_VCPKG=0; shift ;;
        --upload) upload=1; shift ;;
        --version)
            [[ -z "${2:-}" ]] && { echo "[!] --version requires a value" >&2; exit 2; }
            VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[!] Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# Fail fast before burning the dependency build. Every invocation produces a
# store artifact, so a real version is mandatory -- the pbxproj placeholders
# (0.0.0 / 0.0.0.0) must never ship.
if [[ -z "$VERSION" ]]; then
    echo "[!] Fatal: --version W.X.Y.Z is required (should match the release tag)." >&2
    usage >&2
    exit 2
fi
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "[!] Fatal: --version must be W.X.Y.Z (4 dot-separated non-negative integers); got '$VERSION'" >&2
    exit 2
fi
if [[ -z "$APP_PROFILE" || -z "$APPEX_PROFILE" ]]; then
    echo "[!] Fatal: APPLE_IOS_CLIPP_PROVISIONING_PROFILE and APPLE_IOS_CLIPPSE_PROVISIONING_PROFILE" >&2
    echo "    are both required (paths to the two iOS App Store .mobileprovision files:" >&2
    echo "    $APP_BUNDLE_ID and $APPEX_BUNDLE_ID have distinct bundle ids, and App Groups" >&2
    echo "    rules out a wildcard profile)." >&2
    exit 2
fi
for p in "$APP_PROFILE" "$APPEX_PROFILE"; do
    if [[ ! -f "$p" ]]; then
        echo "[!] Fatal: provisioning profile path does not point at a readable file: $p" >&2
        exit 2
    fi
done
: "${APPLE_API_KEY_PATH:?requires APPLE_API_KEY_PATH (path to .p8)}"
: "${APPLE_API_KEY_ID:?requires APPLE_API_KEY_ID}"
: "${APPLE_API_ISSUER_ID:?requires APPLE_API_ISSUER_ID}"
if [[ ! -f "$APPLE_API_KEY_PATH" ]]; then
    echo "[!] Fatal: APPLE_API_KEY_PATH does not point at a readable file: $APPLE_API_KEY_PATH" >&2
    exit 2
fi
# xcodebuild resolves the key path relative to its own cwd quirks; absolutize.
APPLE_API_KEY_PATH="$(cd -- "$(dirname -- "$APPLE_API_KEY_PATH")" && pwd)/$(basename -- "$APPLE_API_KEY_PATH")"

# Manual export signing needs the distribution identity's PRIVATE KEY in the
# keychain -- a portal-listed cert alone is not enough. Catch it now.
if ! security find-identity -v -p codesigning 2>/dev/null | grep -qF "$SIGNING_IDENTITY"; then
    echo "[!] Fatal: no '$SIGNING_IDENTITY' signing identity in the keychain." >&2
    echo "    Create one: Keychain Access -> Certificate Assistant -> Request a" >&2
    echo "    Certificate From a Certificate Authority (save to disk), upload the" >&2
    echo "    CSR at developer.apple.com -> Certificates -> + -> Apple Distribution," >&2
    echo "    download the .cer and double-click it. Or set APPLE_CODESIGN_IDENTITY_IOS" >&2
    echo "    if your identity is named differently (security find-identity -v -p codesigning)." >&2
    exit 2
fi

need_tool() {
    command -v "$1" >/dev/null 2>&1 || { echo "[!] Fatal: $1 is required but was not found in PATH." >&2; exit 1; }
}
need_tool xcodebuild
need_tool xcrun
xcrun --sdk iphoneos --show-sdk-path >/dev/null

if [[ "$clean" == "1" ]]; then
    echo "[*] Cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

if [[ "$SETUP_VCPKG" == "1" ]]; then
    # Device (arm64-ios) static libs. MUST run from a terminal, never as an
    # Xcode build phase (Xcode's env poisons vcpkg) -- which this is.
    echo "[*] Setting up iOS device vcpkg dependencies..."
    # Via bash, not direct exec: survives a checkout that lost the executable
    # bit (the repo is developed on Windows; CI checkouts honor index modes).
    bash "$SCRIPT_DIR/setup_ios_vcpkg.sh" --device-only
fi

mkdir -p "$BUILD_DIR"

# Decode a profile, sanity-check it against the bundle id it is supposed to
# sign (a swapped/wrong-app profile dies here in seconds, not as ITMS-90286
# after a full build), and install it where xcodebuild's manual signing looks
# (the classic location plus the Xcode 16+ one; filename convention is the
# UUID). Sets INSTALLED_PROFILE_NAME for the exportOptions mapping.
install_profile() {
    local path="$1" expected_bundle_id="$2" label="$3"
    local decoded="$BUILD_DIR/profile-$label.plist"
    security cms -D -i "$path" -o "$decoded"
    local name uuid app_id
    name="$(/usr/libexec/PlistBuddy -c 'Print :Name' "$decoded")"
    uuid="$(/usr/libexec/PlistBuddy -c 'Print :UUID' "$decoded")"
    app_id="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:application-identifier' "$decoded" 2>/dev/null || true)"
    if [[ -z "$app_id" ]]; then
        app_id="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.application-identifier' "$decoded" 2>/dev/null || true)"
    fi
    # Suffix match (TEAMID.<bundle-id>); the two bundle ids are not suffixes
    # of each other, so a swap of the two env vars is caught in both directions.
    if [[ "$app_id" != *".$expected_bundle_id" ]]; then
        echo "[!] Fatal: the $label provisioning profile is not for $expected_bundle_id." >&2
        echo "    Profile's application-identifier: $app_id" >&2
        echo "    Expected suffix:                  .$expected_bundle_id" >&2
        echo "    Check the profile path passed for '$label': $path" >&2
        exit 1
    fi
    echo "[*] $label distribution profile: '$name' ($uuid)"
    local dir
    for dir in \
        "$HOME/Library/MobileDevice/Provisioning Profiles" \
        "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"
    do
        mkdir -p "$dir"
        cp "$path" "$dir/$uuid.mobileprovision"
    done
    INSTALLED_PROFILE_NAME="$name"
}

install_profile "$APP_PROFILE" "$APP_BUNDLE_ID" "app"
APP_PROFILE_NAME="$INSTALLED_PROFILE_NAME"
install_profile "$APPEX_PROFILE" "$APPEX_BUNDLE_ID" "share-extension"
APPEX_PROFILE_NAME="$INSTALLED_PROFILE_NAME"

# Upload-only auth (any API-key role). The archive step additionally gets
# -allowProvisioningUpdates so automatic DEV signing can refresh profiles;
# the export step deliberately does NOT, so cloud signing is never attempted.
AUTH_ARGS=(
    -authenticationKeyPath "$APPLE_API_KEY_PATH"
    -authenticationKeyID "$APPLE_API_KEY_ID"
    -authenticationKeyIssuerID "$APPLE_API_ISSUER_ID"
)

VERSION_ARGS=(
    MARKETING_VERSION="${VERSION%.*}"
    CURRENT_PROJECT_VERSION="$VERSION"
)
echo "[*] Version: MARKETING_VERSION=${VERSION%.*} CURRENT_PROJECT_VERSION=$VERSION"

echo "[*] Archiving ($CONFIG, generic iOS device)..."
rm -rf "$ARCHIVE_PATH"
xcodebuild archive \
    -project "$PROJECT" \
    -scheme "$SCHEME" \
    -configuration "$CONFIG" \
    -destination 'generic/platform=iOS' \
    -archivePath "$ARCHIVE_PATH" \
    -derivedDataPath "$BUILD_DIR/DerivedData" \
    -allowProvisioningUpdates \
    "${AUTH_ARGS[@]}" \
    "${VERSION_ARGS[@]}"

if [[ ! -d "$ARCHIVE_PATH" ]]; then
    echo "[!] Fatal: archive not found at $ARCHIVE_PATH" >&2
    exit 1
fi

ARCHIVED_APP="$ARCHIVE_PATH/Products/Applications/Clipp.app"
ARCHIVED_APPEX="$ARCHIVED_APP/PlugIns/ClippShareExtension.appex"

# The bundle-id assumptions above must hold for the archived products too, and
# the share extension must actually be embedded.
ARCHIVED_BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$ARCHIVED_APP/Info.plist")"
if [[ "$ARCHIVED_BUNDLE_ID" != "$APP_BUNDLE_ID" ]]; then
    echo "[!] Fatal: archived app bundle id '$ARCHIVED_BUNDLE_ID' != expected '$APP_BUNDLE_ID'." >&2
    exit 1
fi
if [[ ! -d "$ARCHIVED_APPEX" ]]; then
    echo "[!] Fatal: share extension not embedded in the archive: $ARCHIVED_APPEX" >&2
    exit 1
fi
ARCHIVED_APPEX_BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$ARCHIVED_APPEX/Info.plist")"
if [[ "$ARCHIVED_APPEX_BUNDLE_ID" != "$APPEX_BUNDLE_ID" ]]; then
    echo "[!] Fatal: archived extension bundle id '$ARCHIVED_APPEX_BUNDLE_ID' != expected '$APPEX_BUNDLE_ID'." >&2
    exit 1
fi

# Belt-and-braces: the privacy manifest must sit at the app bundle root for
# App Store validation; cheaper to fail here. It lives in the Clipp
# file-system-synchronized folder (ios/Clipp/PrivacyInfo.xcprivacy), so Xcode
# bundles it automatically -- this catches it falling out of the folder.
if [[ ! -f "$ARCHIVED_APP/PrivacyInfo.xcprivacy" ]]; then
    echo "[!] Fatal: PrivacyInfo.xcprivacy missing from the archived app bundle root." >&2
    echo "    Expected: $ARCHIVED_APP/PrivacyInfo.xcprivacy" >&2
    exit 1
fi

# The plists reference $(MARKETING_VERSION)/$(CURRENT_PROJECT_VERSION); assert
# the command-line overrides actually landed in BOTH bundles (App Store
# validation also requires the two CFBundleShortVersionStrings to match).
for bundle in "$ARCHIVED_APP" "$ARCHIVED_APPEX"; do
    got_full="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$bundle/Info.plist")"
    got_short="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$bundle/Info.plist")"
    if [[ "$got_full" != "$VERSION" || "$got_short" != "${VERSION%.*}" ]]; then
        echo "[!] Fatal: $bundle is stamped '$got_short'/'$got_full', expected '${VERSION%.*}'/'$VERSION'." >&2
        exit 1
    fi
done
echo "[*] Archive stamped ${VERSION%.*} / $VERSION (app + share extension)."

if [[ "$upload" == "1" ]]; then
    DESTINATION="upload"
else
    DESTINATION="export"
fi

EXPORT_OPTIONS="$BUILD_DIR/exportOptions.plist"
cat > "$EXPORT_OPTIONS" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>method</key>
    <string>app-store-connect</string>
    <key>destination</key>
    <string>$DESTINATION</string>
    <key>signingStyle</key>
    <string>manual</string>
    <key>signingCertificate</key>
    <string>$SIGNING_IDENTITY</string>
    <key>provisioningProfiles</key>
    <dict>
        <key>$APP_BUNDLE_ID</key>
        <string>$APP_PROFILE_NAME</string>
        <key>$APPEX_BUNDLE_ID</key>
        <string>$APPEX_PROFILE_NAME</string>
    </dict>
    <key>uploadSymbols</key>
    <true/>
</dict>
</plist>
EOF

if [[ "$upload" == "1" ]]; then
    echo "[*] Exporting + uploading to App Store Connect (may take several minutes)..."
else
    echo "[*] Exporting signed .ipa to $EXPORT_PATH ..."
fi
rm -rf "$EXPORT_PATH"
xcodebuild -exportArchive \
    -archivePath "$ARCHIVE_PATH" \
    -exportOptionsPlist "$EXPORT_OPTIONS" \
    -exportPath "$EXPORT_PATH" \
    "${AUTH_ARGS[@]}"

if [[ "$upload" == "1" ]]; then
    echo "[*] Upload complete. Check App Store Connect / TestFlight for processing status."
else
    echo "[*] Export complete:"
    ls -la "$EXPORT_PATH"
    echo "[*] Ship it later with: $(basename "$0") --version $VERSION --skip-vcpkg --upload   (rebuilds; or upload this .ipa via Transporter)"
fi
echo "[*] Archive kept at: $ARCHIVE_PATH"
