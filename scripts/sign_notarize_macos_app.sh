#!/bin/bash
#
# Sign + notarize an arbitrary macOS .app bundle, using the same flow as
# build_macos.sh's release path (codesign --options=runtime --timestamp,
# ditto-zip, notarytool submit --wait, stapler staple). Standalone: it takes
# any .app, not just Clipp's.
#
# Credentials come from the environment — the same vars the release workflow
# (_release.yml) injects:
#
#   APPLE_CODESIGN_IDENTITY   Developer ID Application identity (name or SHA-1).
#                             Required. May be overridden with --identity.
#
#   Notarization (unless --no-notarize) additionally needs the App Store
#   Connect API key triple:
#   APPLE_API_KEY_PATH        Path to the AuthKey_XXXX.p8 file.
#   APPLE_API_KEY_ID          10-char key id.
#   APPLE_API_ISSUER_ID       Team issuer UUID.
#
# Usage:
#   sign_notarize_macos_app.sh [options] /path/to/Foo.app
#
# Options:
#   --entitlements PATH   Entitlements plist to embed. If omitted, the app is
#                         signed with the hardened runtime only — which already
#                         yields get-task-allow=false, the same secure default
#                         Clipp.entitlements sets. Supply this for apps that
#                         need specific entitlements (JIT, network, etc.).
#   --output PATH         Where to write the notarization/distribution .zip.
#                         Default: <app-dir>/<AppName>.zip
#   --identity ID         Override APPLE_CODESIGN_IDENTITY.
#   --no-notarize         Sign + verify + package only; skip the notary round
#                         trip and stapling. Useful for a quick local check.
#   -h, --help            Show this help.
#
# Must run on macOS (codesign, ditto, xcrun notarytool/stapler, spctl).
#
# Signing is done INSIDE-OUT (nested frameworks/helpers first, deepest first, then
# the bundle) rather than with `codesign --deep`. Both apply the hardened runtime
# correctly; inside-out is used because Apple recommends it for distribution and
# the codesign(1) man page calls --deep unsuitable for production signing (it also
# can't give nested code distinct entitlements). Entitlements, if supplied, are
# applied only to the top-level executable. Helpers needing their own entitlements
# are out of scope for a generic script — sign those yourself first, then run this.

set -euo pipefail

# ---- defaults / arg parsing ------------------------------------------------
IDENTITY="${APPLE_CODESIGN_IDENTITY:-}"
ENTITLEMENTS=""
OUTPUT=""
notarize=1
APP_PATH=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [options] /path/to/Foo.app

Sign + notarize an arbitrary macOS .app using credentials from the environment
(the same vars _release.yml injects): APPLE_CODESIGN_IDENTITY and, for
notarization, APPLE_API_KEY_PATH / APPLE_API_KEY_ID / APPLE_API_ISSUER_ID.

Options:
  --entitlements PATH   Entitlements plist to embed. Omit to sign with the
                        hardened runtime only (get-task-allow=false default).
  --output PATH         Destination .zip. Default: <app-dir>/<AppName>.zip
  --identity ID         Override APPLE_CODESIGN_IDENTITY.
  --no-notarize         Sign + verify + package only; skip notary + stapling.
  -h, --help            Show this help.

Must run on macOS.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --entitlements)
            [[ -n "${2:-}" ]] || { echo "[!] --entitlements requires a path" >&2; exit 2; }
            ENTITLEMENTS="$2"; shift 2 ;;
        --entitlements=*) ENTITLEMENTS="${1#--entitlements=}"; shift ;;
        --output)
            [[ -n "${2:-}" ]] || { echo "[!] --output requires a path" >&2; exit 2; }
            OUTPUT="$2"; shift 2 ;;
        --output=*) OUTPUT="${1#--output=}"; shift ;;
        --identity)
            [[ -n "${2:-}" ]] || { echo "[!] --identity requires a value" >&2; exit 2; }
            IDENTITY="$2"; shift 2 ;;
        --identity=*) IDENTITY="${1#--identity=}"; shift ;;
        --no-notarize) notarize=0; shift ;;
        -h|--help) usage; exit 0 ;;
        --*) echo "[!] Unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)
            if [[ -n "$APP_PATH" ]]; then
                echo "[!] Unexpected extra argument: $1 (already have app: $APP_PATH)" >&2
                exit 2
            fi
            APP_PATH="$1"; shift ;;
    esac
done

# ---- validate inputs -------------------------------------------------------
if [[ -z "$APP_PATH" ]]; then
    echo "[!] Fatal: no .app path given." >&2
    usage >&2
    exit 2
fi
APP_PATH="${APP_PATH%/}"   # tolerate trailing slash from tab-completion

if [[ ! -d "$APP_PATH" ]]; then
    echo "[!] Fatal: not a directory: $APP_PATH" >&2
    exit 2
fi
if [[ "$APP_PATH" != *.app ]]; then
    echo "[!] Fatal: expected a path ending in .app, got: $APP_PATH" >&2
    exit 2
fi

if [[ -z "$IDENTITY" ]]; then
    echo "[!] Fatal: signing identity required. Set APPLE_CODESIGN_IDENTITY or pass --identity." >&2
    exit 2
fi

if [[ -n "$ENTITLEMENTS" && ! -f "$ENTITLEMENTS" ]]; then
    echo "[!] Fatal: --entitlements file not found: $ENTITLEMENTS" >&2
    exit 2
fi

# Fail fast on a misconfigured notarize before we burn time signing.
if [[ "$notarize" == "1" ]]; then
    : "${APPLE_API_KEY_PATH:?--notarize requires APPLE_API_KEY_PATH (path to .p8). Use --no-notarize to skip.}"
    : "${APPLE_API_KEY_ID:?--notarize requires APPLE_API_KEY_ID. Use --no-notarize to skip.}"
    : "${APPLE_API_ISSUER_ID:?--notarize requires APPLE_API_ISSUER_ID. Use --no-notarize to skip.}"
    if [[ ! -f "$APPLE_API_KEY_PATH" ]]; then
        echo "[!] Fatal: APPLE_API_KEY_PATH does not point at a readable file: $APPLE_API_KEY_PATH" >&2
        exit 2
    fi
fi

command -v codesign >/dev/null 2>&1 || { echo "[!] Fatal: codesign not found. This script must run on macOS." >&2; exit 1; }
command -v xcrun    >/dev/null 2>&1 || { echo "[!] Fatal: xcrun not found. Install the Xcode Command Line Tools." >&2; exit 1; }

APP_NAME="$(basename "$APP_PATH" .app)"
APP_DIR="$(cd -- "$(dirname -- "$APP_PATH")" && pwd)"
APP_PATH="$APP_DIR/$(basename "$APP_PATH")"   # absolute, for clean log output
ZIP_PATH="${OUTPUT:-$APP_DIR/$APP_NAME.zip}"

if [[ "$notarize" == "1" ]]; then NOTARIZE_LABEL="yes"; else NOTARIZE_LABEL="no"; fi
echo "[*] App:          $APP_PATH"
echo "[*] Identity:     $IDENTITY"
echo "[*] Entitlements: ${ENTITLEMENTS:-<none - hardened runtime defaults>}"
echo "[*] Output zip:   $ZIP_PATH"
echo "[*] Notarize:     $NOTARIZE_LABEL"

# ---- sign (inside-out) -----------------------------------------------------
# Sign nested code first (deepest first), then the bundle itself. Both this and
# `codesign --deep` apply the hardened runtime correctly, but Apple recommends
# inside-out for distribution/notarization: --deep can't give nested code distinct
# entitlements and the codesign(1) man page calls it unsuitable for production
# signing. (Pass nothing special; the only behavioural difference you'll notice
# vs --deep is that each nested item is timestamped on its own.)
#
# --force re-signs over any existing signature; --options=runtime opts into the
# hardened runtime (a notarization prerequisite); --timestamp pulls a secure
# timestamp from Apple. Entitlements, if supplied, go on the top-level executable
# only — nested libraries are hardened but get no entitlements (a generic script
# can't know a helper's specific entitlement needs).
echo "[*] Enumerating nested code..."
# Match nested bundles by name and every other file by content (file(1) Mach-O
# check), deepest-first so children are signed before their containing bundle.
# The outer \( ... \) groups both branches under -print0 (without it, -print0
# would bind only to the -type f branch and skip the bundle dirs).
declare -a NESTED=()
while IFS= read -r -d '' item; do
    if [[ -d "$item" ]]; then
        NESTED+=("$item")
    # Capture file(1)'s output and glob-match it instead of `| grep -q`: under
    # `set -o pipefail`, grep -q's early exit can SIGPIPE file(1) and flip the
    # branch (same trap as the hardened-runtime check below).
    elif [[ "$(file -b "$item" 2>/dev/null)" == *Mach-O* ]]; then
        NESTED+=("$item")
    fi
done < <(
    find "$APP_PATH/Contents" -depth \( \
        \( -type d \( -name '*.framework' -o -name '*.app' -o -name '*.xpc' \
                      -o -name '*.appex' -o -name '*.bundle' -o -name '*.plugin' \) \) \
        -o -type f \
    \) -print0
)

echo "[*] Signing ${#NESTED[@]} nested item(s) with the hardened runtime..."
# Guard the expansion: macOS's default /bin/bash is 3.2, where "${arr[@]}" on an
# empty array under `set -u` is an "unbound variable" error (simple apps with no
# nested code hit this).
if [[ ${#NESTED[@]} -gt 0 ]]; then
    for item in "${NESTED[@]}"; do
        echo "    - ${item#"$APP_PATH/"}"
        codesign --force --options=runtime --timestamp --sign "$IDENTITY" "$item"
    done
fi

echo "[*] Signing the app bundle..."
SIGN_ARGS=(--force --options=runtime --timestamp)
if [[ -n "$ENTITLEMENTS" ]]; then
    SIGN_ARGS+=(--entitlements "$ENTITLEMENTS")
fi
codesign "${SIGN_ARGS[@]}" --sign "$IDENTITY" "$APP_PATH"

# ---- verify ----------------------------------------------------------------
echo "[*] Verifying signature..."
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

# Confirm the hardened-runtime flag actually landed on the top-level executable;
# cheaper to fail here than minutes into a notary round trip.
#
# Capture codesign's output into a variable and match with a here-string rather
# than piping straight into `grep -q`. Under `set -o pipefail`, `codesign | grep -q`
# is a trap: grep finds the flag and exits immediately, codesign gets SIGPIPE (141)
# while still writing, and pipefail then reports the whole pipeline as failed even
# though the flag IS present — a false negative that rejects a perfectly good
# signature. A here-string has no upstream process to SIGPIPE, so it's safe.
CODESIGN_INFO="$(codesign --display --verbose=4 "$APP_PATH" 2>&1 || true)"
if ! grep -Eq 'flags=[^[:space:]]*runtime' <<<"$CODESIGN_INFO"; then
    echo "[!] hardened runtime flag not detected on $APP_PATH. Actual CodeDirectory flags:" >&2
    grep -E 'flags=' <<<"$CODESIGN_INFO" >&2 || true
    if [[ "$notarize" == "1" ]]; then
        echo "[!] Fatal: notarization would be rejected without the hardened runtime." >&2
        exit 1
    fi
    echo "[!] Warning: continuing without hardened runtime (sign-only mode)." >&2
fi

# ---- package ---------------------------------------------------------------
# ditto (not zip) preserves macOS metadata; --sequesterRsrc keeps the archive
# shape notarytool expects; --keepParent puts <App>.app at the archive root.
echo "[*] Packaging: $ZIP_PATH"
rm -f "$ZIP_PATH"
mkdir -p "$(dirname "$ZIP_PATH")"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$APP_PATH" "$ZIP_PATH"

if [[ "$notarize" != "1" ]]; then
    echo "[*] Done (sign-only). Signed bundle: $APP_PATH"
    echo "[*] Archive (not notarized): $ZIP_PATH"
    exit 0
fi

# ---- notarize --------------------------------------------------------------
echo "[*] Submitting to Apple's notary service (this can take a few minutes)..."
NOTARY_LOG="$APP_DIR/$APP_NAME.notarytool-submit.log"
set +e
xcrun notarytool submit "$ZIP_PATH" \
    --key "$APPLE_API_KEY_PATH" \
    --key-id "$APPLE_API_KEY_ID" \
    --issuer "$APPLE_API_ISSUER_ID" \
    --wait 2>&1 | tee "$NOTARY_LOG"
NOTARY_STATUS=${PIPESTATUS[0]}
set -e

SUBMISSION_ID="$(grep -E '^[[:space:]]*id:' "$NOTARY_LOG" | head -n1 | awk '{print $2}' || true)"

# notarytool's exit code reflects upload+protocol success, not the verdict.
# The verdict is the final "status:" line — Accepted is the only good one.
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

# ---- staple ----------------------------------------------------------------
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

echo "[*] Done. Signed + notarized + stapled: $APP_PATH"
echo "[*] Distributable archive: $ZIP_PATH"
