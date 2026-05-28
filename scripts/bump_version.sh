#!/bin/bash
# Bumps the project version across all files that hold it.
#
# Single source of truth for the C++/CMake side is the set(CLIPP_VERSION "...")
# default in CMakeLists.txt; iOS reads its CFBundle* keys at runtime from
# Info.plist so the two iOS plists need a separate poke.
#
# Run from macOS (uses /usr/libexec/PlistBuddy). The script does NOT commit or
# tag for you — review with `git diff` first.

set -euo pipefail

usage() {
    cat >&2 <<EOF
Usage: $(basename "$0") W.X.Y.Z

Bumps the project version. Touches:
  CMakeLists.txt                      (CLIPP_VERSION default)
  ios/Info.plist                      (CFBundleShortVersionString + CFBundleVersion)
  ios/ClippShareExtension/Info.plist  (same)

CFBundleShortVersionString uses the 3-part form (W.X.Y) since Apple rejects
4-part values there. CFBundleVersion uses the full 4-part.
EOF
}

if [[ $# -ne 1 ]]; then
    usage
    exit 2
fi

if ! [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "[!] Version must be W.X.Y.Z (4 dot-separated non-negative integers); got '$1'" >&2
    exit 2
fi

VERSION="$1"
VERSION_3PART="${VERSION%.*}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

PLISTBUDDY=/usr/libexec/PlistBuddy
if [[ ! -x "$PLISTBUDDY" ]]; then
    echo "[!] $PLISTBUDDY not found; this script must be run from macOS." >&2
    exit 1
fi

CMAKE_FILE="CMakeLists.txt"
IOS_PLIST="ios/Info.plist"
IOS_SHARE_PLIST="ios/ClippShareExtension/Info.plist"

for f in "$CMAKE_FILE" "$IOS_PLIST" "$IOS_SHARE_PLIST"; do
    if [[ ! -f "$f" ]]; then
        echo "[!] Missing expected file: $f" >&2
        exit 1
    fi
done

echo "[*] Bumping to $VERSION (CFBundleShortVersionString uses 3-part: $VERSION_3PART)"

# CMakeLists.txt — replace the quoted value in: set(CLIPP_VERSION "X.Y.Z.W")
# BSD sed (macOS) requires an extension after -i; we use .bak and remove it.
sed -i.bak -E "s|(set\\(CLIPP_VERSION )\"[^\"]*\"|\\1\"$VERSION\"|" "$CMAKE_FILE"
rm -f "$CMAKE_FILE.bak"

# Sanity-check the sed actually landed — guards against the default line being
# renamed/restructured without this script noticing.
if ! grep -q "set(CLIPP_VERSION \"$VERSION\")" "$CMAKE_FILE"; then
    echo "[!] sed did not produce the expected line in $CMAKE_FILE." >&2
    echo "    Has the set(CLIPP_VERSION ...) default been renamed?" >&2
    exit 1
fi
echo "  [+] $CMAKE_FILE"

for plist in "$IOS_PLIST" "$IOS_SHARE_PLIST"; do
    "$PLISTBUDDY" -c "Set :CFBundleShortVersionString $VERSION_3PART" "$plist"
    "$PLISTBUDDY" -c "Set :CFBundleVersion $VERSION" "$plist"
    echo "  [+] $plist"
done

cat <<EOF

[*] Done. Review with:
      git diff

[*] Then commit + tag:
      git commit -am "Bump version to $VERSION"
      git tag v$VERSION
EOF
