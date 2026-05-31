#!/bin/bash
set -euo pipefail

# Packages an already-built terminal clipp binary into .deb, .rpm, and Arch
# .pkg.tar.zst via nfpm (https://nfpm.goreleaser.com). One binary in, three packages
# out -- see packaging/nfpm.yaml for metadata and the single avahi dependency.
#
# This does NOT build clipp; run scripts/build_linux.sh first (ideally on the old-glibc
# release base for broad compatibility -- see the CI build-linux job). Locally it
# happily packages a dev build for smoke-testing the package shape.
#
# Usage: package_linux.sh --version W.X.Y.Z [--binary PATH] [--arch ARCH] [--outdir DIR]
#   --binary   path to the built clipp (default: autodetect under build/linux-*/ )
#   --arch     nfpm arch: amd64 | arm64 | arm (default: derived from `uname -m`)
#   --outdir   where packages land (default: ./dist)

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

VERSION=""
BINARY=""
ARCH=""
OUTDIR="$REPO_ROOT/dist"

usage() { sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="${2:-}"; shift 2 ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        --binary) BINARY="${2:-}"; shift 2 ;;
        --binary=*) BINARY="${1#--binary=}"; shift ;;
        --arch) ARCH="${2:-}"; shift 2 ;;
        --arch=*) ARCH="${1#--arch=}"; shift ;;
        --outdir) OUTDIR="${2:-}"; shift 2 ;;
        --outdir=*) OUTDIR="${1#--outdir=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[!] Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$VERSION" ]]; then
    echo "[!] --version W.X.Y.Z is required." >&2
    exit 2
fi

# Locate the binary if not given: prefer release, then any linux-* build tree.
if [[ -z "$BINARY" ]]; then
    for candidate in \
        "$REPO_ROOT"/build/linux-release/clipp \
        "$REPO_ROOT"/build/linux-*/clipp; do
        if [[ -x "$candidate" ]]; then
            BINARY="$candidate"
            break
        fi
    done
fi
if [[ -z "$BINARY" || ! -x "$BINARY" ]]; then
    echo "[!] No clipp binary found. Pass --binary PATH or run scripts/build_linux.sh first." >&2
    exit 1
fi

# Derive nfpm arch from the host if not specified. nfpm uses Go-style names; map the
# common uname -m outputs. (Cross-arch packaging just needs --arch + a matching binary.)
if [[ -z "$ARCH" ]]; then
    case "$(uname -m)" in
        x86_64|amd64) ARCH="amd64" ;;
        aarch64|arm64) ARCH="arm64" ;;
        armv7l|armv6l|arm) ARCH="arm" ;;
        *) echo "[!] Unrecognized machine '$(uname -m)'; pass --arch explicitly." >&2; exit 1 ;;
    esac
fi

# Ensure nfpm is available. Prefer one already on PATH; otherwise fetch a pinned
# release into a local cache so repeated runs are fast and CI is reproducible.
NFPM_BIN="$(command -v nfpm || true)"
if [[ -z "$NFPM_BIN" ]]; then
    NFPM_VERSION_PIN="2.41.3"
    case "$ARCH" in
        amd64) NFPM_DL_ARCH="x86_64" ;;
        arm64) NFPM_DL_ARCH="arm64" ;;
        arm)   NFPM_DL_ARCH="armv6" ;;
    esac
    CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/clipp/nfpm-$NFPM_VERSION_PIN"
    NFPM_BIN="$CACHE_DIR/nfpm"
    if [[ ! -x "$NFPM_BIN" ]]; then
        echo "[*] nfpm not on PATH; downloading v$NFPM_VERSION_PIN ($NFPM_DL_ARCH)..."
        mkdir -p "$CACHE_DIR"
        url="https://github.com/goreleaser/nfpm/releases/download/v${NFPM_VERSION_PIN}/nfpm_${NFPM_VERSION_PIN}_Linux_${NFPM_DL_ARCH}.tar.gz"
        curl -fsSL "$url" | tar -xz -C "$CACHE_DIR" nfpm
        chmod +x "$NFPM_BIN"
    fi
fi
echo "[*] Using nfpm: $NFPM_BIN ($("$NFPM_BIN" --version 2>/dev/null | head -n1 || echo '?'))"

CONFIG="$REPO_ROOT/packaging/nfpm.yaml"

# nfpm does NOT reliably expand ${VAR} in every config field (the contents[].src glob
# comes through literal), so render the template into a concrete config ourselves. We
# restrict substitution to our three known vars so any other '$' in the file is left
# alone. envsubst (gettext-base) is the clean tool; perl is the universal fallback.
export CLIPP_BINARY="$BINARY"
export NFPM_VERSION="$VERSION"
export NFPM_ARCH="$ARCH"

# Render config and stage output on LOCAL disk: OUTDIR may be a CIFS/SMB mount where
# nfpm's write-temp-then-rename is unreliable (same hazard that made build_linux.sh
# relocate the build tree). We copy the finished packages to OUTDIR afterward -- a
# plain sequential write, which the share handles fine.
RENDERED_CONFIG="$(mktemp -t clipp-nfpm-XXXXXX.yaml)"
STAGE_DIR="$(mktemp -d -t clipp-pkg-XXXXXX)"
trap 'rm -rf "$RENDERED_CONFIG" "$STAGE_DIR"' EXIT

if command -v envsubst >/dev/null 2>&1; then
    envsubst '${CLIPP_BINARY} ${NFPM_VERSION} ${NFPM_ARCH}' < "$CONFIG" > "$RENDERED_CONFIG"
elif command -v perl >/dev/null 2>&1; then
    perl -pe 's/\$\{(\w+)\}/defined $ENV{$1} ? $ENV{$1} : $&/ge' < "$CONFIG" > "$RENDERED_CONFIG"
else
    echo "[!] Need envsubst (gettext-base package) or perl to render the nfpm config." >&2
    exit 1
fi

echo "[*] Packaging clipp $VERSION ($ARCH) from $BINARY"
echo "[*] Output: $OUTDIR"

# One package per format. Tolerate a single packager failing (Arch is the least-
# exercised path in nfpm) so it can't rob you of a good .deb/.rpm; report at the end.
overall_rc=0
for packager in deb rpm archlinux; do
    echo "[*] -> $packager"
    if ! "$NFPM_BIN" package --config "$RENDERED_CONFIG" --packager "$packager" --target "$STAGE_DIR/"; then
        echo "[!] nfpm failed for packager '$packager' (continuing with the rest)." >&2
        overall_rc=1
    fi
done

mkdir -p "$OUTDIR"
shopt -s nullglob
staged=("$STAGE_DIR"/*)
shopt -u nullglob
if (( ${#staged[@]} == 0 )); then
    echo "[!] No packages were produced." >&2
    exit 1
fi
cp -f "${staged[@]}" "$OUTDIR"/

echo "[*] Done. Packages in $OUTDIR:"
ls -1 "$OUTDIR"
exit $overall_rc
