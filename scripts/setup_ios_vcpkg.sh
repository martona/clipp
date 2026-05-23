#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

if [[ -z "${CLIPP_CACHE_DIR:-}" ]]; then
    if [[ -n "${XDG_CACHE_HOME:-}" ]]; then
        CLIPP_CACHE_DIR="$XDG_CACHE_HOME/clipp"
    else
        CLIPP_CACHE_DIR="$HOME/Library/Caches/clipp"
    fi
fi

VCPKG_ROOT="${VCPKG_ROOT:-$CLIPP_CACHE_DIR/vcpkg}"
VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-$CLIPP_CACHE_DIR/vcpkg-binary-cache}"
VCPKG_INSTALLED_DIR="${VCPKG_INSTALLED_DIR:-$REPO_ROOT/vcpkg-installed}"
VCPKG_STAGING_INSTALLED_DIR="${VCPKG_STAGING_INSTALLED_DIR:-$CLIPP_CACHE_DIR/vcpkg-ios-installed}"
VCPKG_OVERLAY_TRIPLETS="${VCPKG_OVERLAY_TRIPLETS:-$REPO_ROOT/src/vcpkg-triplets}"
IOS_DEVICE_TRIPLET="${CLIPP_IOS_DEVICE_TRIPLET:-arm64-ios}"
IOS_SIMULATOR_TRIPLET="${CLIPP_IOS_SIMULATOR_TRIPLET:-arm64-ios-simulator}"

export VCPKG_ROOT
export VCPKG_DEFAULT_BINARY_CACHE
export VCPKG_OVERLAY_TRIPLETS
export VCPKG_BINARY_SOURCES="${VCPKG_BINARY_SOURCES:-clear;files,$VCPKG_DEFAULT_BINARY_CACHE,readwrite}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--device-only] [--simulator-only] [--triplet TRIPLET] [--clean]

Installs iOS vcpkg dependencies into:
  $VCPKG_INSTALLED_DIR

Uses per-triplet staging roots under:
  $VCPKG_STAGING_INSTALLED_DIR

Default triplets:
  device:    $IOS_DEVICE_TRIPLET
  simulator: $IOS_SIMULATOR_TRIPLET
EOF
}

install_device=1
install_simulator=1
custom_triplets=()
clean=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device-only)
            install_device=1
            install_simulator=0
            shift
            ;;
        --simulator-only)
            install_device=0
            install_simulator=1
            shift
            ;;
        --triplet)
            [[ $# -ge 2 ]] || { echo "[!] --triplet requires a value." >&2; exit 2; }
            custom_triplets+=("$2")
            shift 2
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
        return 1
    }
}

need_tool git
need_tool xcrun
need_tool ditto

if ! command -v autoconf >/dev/null 2>&1 \
    || ! command -v automake >/dev/null 2>&1 \
    || ! command -v autoreconf >/dev/null 2>&1 \
    || { ! command -v libtoolize >/dev/null 2>&1 && ! command -v glibtoolize >/dev/null 2>&1; }; then
    cat >&2 <<'EOF'
[!] Missing autotools required by libsodium's vcpkg port.
    With MacPorts:
      sudo port install autoconf autoconf-archive automake libtool
    With Homebrew:
      brew install autoconf autoconf-archive automake libtool
EOF
    exit 1
fi

xcrun --sdk iphoneos --show-sdk-path >/dev/null
xcrun --sdk iphonesimulator --show-sdk-path >/dev/null

if [[ "$clean" == "1" ]]; then
    echo "[*] Cleaning iOS vcpkg install root: $VCPKG_INSTALLED_DIR"
    rm -rf "$VCPKG_INSTALLED_DIR"
    echo "[*] Cleaning iOS vcpkg staging root: $VCPKG_STAGING_INSTALLED_DIR"
    rm -rf "$VCPKG_STAGING_INSTALLED_DIR"
fi

mkdir -p "$(dirname "$VCPKG_ROOT")" "$VCPKG_DEFAULT_BINARY_CACHE" "$VCPKG_INSTALLED_DIR" "$VCPKG_STAGING_INSTALLED_DIR"

TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
    if [[ ! -e "$VCPKG_ROOT" || -z "$(ls -A "$VCPKG_ROOT" 2>/dev/null)" ]]; then
        echo "[*] Cloning vcpkg into $VCPKG_ROOT..."
        git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
    else
        echo "[!] Fatal: $VCPKG_ROOT exists but does not look like a vcpkg checkout." >&2
        exit 1
    fi
fi

if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
    echo "[*] Bootstrapping vcpkg..."
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

triplets=()
if [[ "${#custom_triplets[@]}" -gt 0 ]]; then
    triplets=("${custom_triplets[@]}")
else
    [[ "$install_device" == "1" ]] && triplets+=("$IOS_DEVICE_TRIPLET")
    [[ "$install_simulator" == "1" ]] && triplets+=("$IOS_SIMULATOR_TRIPLET")
fi

extra_options=(--clean-buildtrees-after-build --clean-packages-after-build)
if [[ -n "${VCPKG_INSTALL_OPTIONS:-}" ]]; then
    extra_options=()
    IFS=';' read -r -a requested_options <<< "$VCPKG_INSTALL_OPTIONS"
    for option in "${requested_options[@]}"; do
        [[ -n "$option" ]] && extra_options+=("$option")
    done
fi

for triplet in "${triplets[@]}"; do
    echo "[*] Installing vcpkg manifest for $triplet..."
    triplet_install_root="$VCPKG_STAGING_INSTALLED_DIR/$triplet"
    (
        cd "$REPO_ROOT/src"
        "$VCPKG_ROOT/vcpkg" install \
            --triplet "$triplet" \
            --overlay-triplets "$VCPKG_OVERLAY_TRIPLETS" \
            --x-install-root="$triplet_install_root" \
            "${extra_options[@]}"
    )

    triplet_source_dir="$triplet_install_root/$triplet"
    triplet_target_dir="$VCPKG_INSTALLED_DIR/$triplet"
    [[ -d "$triplet_source_dir" ]] || {
        echo "[!] Fatal: expected vcpkg output was not created: $triplet_source_dir" >&2
        exit 1
    }

    echo "[*] Publishing $triplet to $triplet_target_dir..."
    rm -rf "$triplet_target_dir"
    ditto "$triplet_source_dir" "$triplet_target_dir"
done

echo "[*] iOS vcpkg dependencies are ready under $VCPKG_INSTALLED_DIR"
