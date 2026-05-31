#!/bin/bash
set -e

# Builds the terminal-only Linux binary (clipp). No GUI, no tray, no local
# clipboard I/O, no background sync daemon -- just the CLI verbs copy/paste/key/
# hostid (the CLIPP_HEADLESS target). See project_linux_port / CMakeLists.txt
# (the `elseif (UNIX AND NOT APPLE)` branch).
#
# Mirrors scripts/build_macos.sh's vcpkg bootstrap + Ninja flow, minus everything
# Apple-specific (codesigning, notarization, bundle packaging).

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

CONFIG="Release"
VERSION=""
clean=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--debug | --release] [--clean] [--version W.X.Y.Z]

Builds the terminal-only Linux clipp binary.

Options:
  --debug          Build the Debug configuration.
  --release        Build the Release configuration (default).
  --clean          Remove the build directory before building.
  --version VER    Stamp the binary with this version (W.X.Y.Z). Omit to let
                   CMake default to 0.0.0.0 (local dev builds).

Runtime note: peer discovery needs a running avahi-daemon. Without it, key/hostid
still work but copy/paste fail loudly ("could not reach any device").
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) CONFIG="Debug"; shift ;;
        --release) CONFIG="Release"; shift ;;
        --clean) clean=1; shift ;;
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

CONFIG_LOWER="$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
# Default build tree lives in-source: build/linux-<config>/, binary at its root.
BUILD_DIR="build/linux-$CONFIG_LOWER"

# CIFS/SMB shares reject the chmod that CMake's configure_file performs (it copies
# source permission bits onto its output), and it fails on CMake's OWN internal
# probe files -- "configure_file: Operation not permitted" during compiler
# detection -- not just ours, so it cannot be worked around in CMakeLists. Detect
# the condition empirically (try to chmod a temp file in the build location) and,
# if it fails, relocate the build tree to local disk under CLIPP_CACHE_DIR. The
# source can stay on the share; only the build tree must be chmod-capable.
# (Same root cause as the documented macOS-on-SMB staging issue.)
CLIPP_CACHE_DIR="${CLIPP_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/clipp}"
build_tree_supports_chmod() {
    local dir="$1"
    mkdir -p "$dir" 2>/dev/null || return 1
    local probe="$dir/.chmod-probe.$$"
    ( : > "$probe" ) 2>/dev/null || return 1
    local ok=0
    chmod 600 "$probe" 2>/dev/null || ok=1
    rm -f "$probe" 2>/dev/null
    return $ok
}

if ! build_tree_supports_chmod "$BUILD_DIR"; then
    # Derive a stable per-repo subdir so distinct checkouts don't collide on disk.
    repo_slug="$(echo "$REPO_ROOT" | tr -c 'A-Za-z0-9' '_' | sed 's/^_*//;s/_*$//')"
    RELOCATED_BUILD_DIR="$CLIPP_CACHE_DIR/build/$repo_slug/linux-$CONFIG_LOWER"
    echo "[!] Build location '$BUILD_DIR' does not support chmod (CIFS/SMB mount?)."
    echo "    CMake's configure_file would fail there. Relocating the build tree to"
    echo "    local disk: $RELOCATED_BUILD_DIR"
    BUILD_DIR="$RELOCATED_BUILD_DIR"
fi

if [[ "$clean" == "1" ]]; then
    echo "[*] Clean flag detected. Removing $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

# --- Preflight: check (don't auto-install) the system prerequisites ----------
# Unlike the macOS script (brew needs no sudo), Linux installs need sudo and vary
# by distro, so we check and instruct rather than installing.

# Best-effort package-manager hint for whatever's missing.
install_hint() {
    local pkgs="$1"
    if command -v apt-get &>/dev/null; then
        echo "    sudo apt-get install -y $pkgs"
    elif command -v dnf &>/dev/null; then
        echo "    sudo dnf install -y $pkgs"
    elif command -v pacman &>/dev/null; then
        echo "    sudo pacman -S --needed $pkgs"
    else
        echo "    (install with your distro's package manager): $pkgs"
    fi
}

missing=0
need_tool() {
    if ! command -v "$1" &>/dev/null; then
        echo "[!] Missing required tool: $1" >&2
        missing=1
    fi
}

echo "[*] Checking build prerequisites..."
need_tool cmake
need_tool ninja
need_tool pkg-config
need_tool git
need_tool curl
# A C++ compiler under any of the usual names.
if ! command -v c++ &>/dev/null && ! command -v g++ &>/dev/null && ! command -v clang++ &>/dev/null; then
    echo "[!] Missing required tool: a C++ compiler (g++ or clang++)" >&2
    missing=1
fi

# Avahi client dev package -- CMake does pkg_check_modules(AVAHI REQUIRED avahi-client),
# so a missing dev package fails configure with a less obvious message. Catch it here.
if ! pkg-config --exists avahi-client 2>/dev/null; then
    echo "[!] Missing required dev package: avahi-client (pkg-config module 'avahi-client')" >&2
    missing=1
fi

if [[ "$missing" == "1" ]]; then
    echo "[!] Install the prerequisites and retry. On a Debian/Ubuntu box:" >&2
    install_hint "build-essential cmake ninja-build pkg-config git curl zip unzip tar autoconf autoconf-archive automake libtool libavahi-client-dev" >&2
    echo "    (autoconf/automake/libtool are needed by vcpkg to build libsodium from source.)" >&2
    exit 1
fi

# --- vcpkg bootstrap (mirrors build_macos.sh) --------------------------------
# CLIPP_CACHE_DIR was already resolved above (for the chmod-relocation check).
VCPKG_ROOT="${VCPKG_ROOT:-$CLIPP_CACHE_DIR/vcpkg}"
VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-$CLIPP_CACHE_DIR/vcpkg-binary-cache}"
VCPKG_INSTALLED_DIR="$CLIPP_CACHE_DIR/vcpkg-installed"

export VCPKG_ROOT
export VCPKG_DEFAULT_BINARY_CACHE

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

# --- Configure + build -------------------------------------------------------
CMAKE_ARGS=(
    -DVCPKG_MANIFEST_DIR="$REPO_ROOT/src"
    -DVCPKG_INSTALLED_DIR="$VCPKG_INSTALLED_DIR"
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
    -DVCPKG_INSTALL_OPTIONS="--clean-buildtrees-after-build;--clean-packages-after-build"
)

if [[ -n "$VERSION" ]]; then
    CMAKE_ARGS+=(-DCLIPP_VERSION="$VERSION")
fi

echo "[*] Generating build files ($CONFIG)..."
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" "${CMAKE_ARGS[@]}"

echo "[*] Building clipp..."
cmake --build "$BUILD_DIR"

BINARY_PATH="$BUILD_DIR/clipp"
if [[ ! -x "$BINARY_PATH" ]]; then
    echo "[!] Fatal: expected binary not found at $BINARY_PATH" >&2
    exit 1
fi

echo "[*] Build complete: $BINARY_PATH"
# Release builds split debug symbols into a sidecar (see CMakeLists); mention it so a
# local builder knows the shipped binary is stripped and where the symbols went.
if [[ "$CONFIG" == "Release" && -f "$BINARY_PATH.debug" ]]; then
    echo "[*] Debug symbols: $BINARY_PATH.debug (binary is stripped; gdb finds it via .gnu_debuglink)"
fi
