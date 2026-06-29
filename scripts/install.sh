#!/usr/bin/env bash
#
# Clipp installer for Linux
# -------------------------
# Usage:   curl -fsSL https://clipp.net/install.sh | sudo bash
#    or:   curl -fsSL https://github.com/martona/clipp/releases/latest/download/install.sh | sudo bash
# Source:  https://github.com/martona/clipp  (this file: scripts/install.sh)
#
# This script is the canonical installer: it is shipped as a release asset
# (releases/latest/download/install.sh) and mirrored at https://clipp.net/install.sh.
# Edit it here; the release pipeline stages it into every release.
#
# Installs the terminal client (clipp copy / clipp paste). If your distro uses a
# supported package manager (apt / dnf / zypper / pacman) it installs the native
# package so the Avahi dependency is pulled in automatically. Otherwise it drops
# the static binary in the current directory and tells you what to do next.

set -eu

BASE="https://github.com/martona/clipp/releases/latest/download"

# ---- pretty, noisy output ---------------------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  esc=$(printf '\033')
  C_CYAN="${esc}[36m"; C_GREEN="${esc}[32m"; C_RED="${esc}[31m"; C_DIM="${esc}[90m"; C_RESET="${esc}[0m"
else
  C_CYAN=; C_GREEN=; C_RED=; C_DIM=; C_RESET=
fi
step() { printf '%s==>%s %s\n' "$C_CYAN"  "$C_RESET" "$1"; }
ok()   { printf '%s==>%s %s\n' "$C_GREEN" "$C_RESET" "$1"; }
note() { printf '    %s%s%s\n' "$C_DIM"   "$1" "$C_RESET"; }
die()  {
  printf '%sx%s   %s\n' "$C_RED" "$C_RESET" "$1" >&2
  printf '    %sManual download: https://clipp.net/#download%s\n' "$C_DIM" "$C_RESET" >&2
  exit 1
}

printf '\n  %sClipp%s - secure clipboard sync for trusted devices\n' "$C_CYAN" "$C_RESET"
printf '  %shttps://clipp.net%s\n\n' "$C_DIM" "$C_RESET"

# ---- temp workspace + cleanup ----------------------------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ---- run privileged commands whether or not we're already root -------------
if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi

# ---- download helper (curl or wget) ----------------------------------------
download() { # url dest
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$1" -o "$2"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$2" "$1"
  else
    die "Need curl or wget to download files."
  fi
}

# ---- 1. architecture (bail if unsupported) ---------------------------------
step "Detecting your architecture..."
machine="$(uname -m)"
case "$machine" in
  x86_64|amd64)  arch=amd64 ;;
  aarch64|arm64) arch=arm64 ;;
  *) die "Unsupported architecture '$machine'. Clipp needs x86_64 or aarch64." ;;
esac
note "$machine -> $arch"

# ---- 2. C library: clipp needs glibc >= 2.31 (this also rules out musl) -----
step "Checking the C library..."
ver_ge() { [ "$1" = "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n1)" ]; }
glibc=""
if command -v getconf >/dev/null 2>&1; then
  glibc="$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')" || true
fi
if [ -z "$glibc" ] && command -v ldd >/dev/null 2>&1; then
  line="$(ldd --version 2>&1 | head -n1)"
  case "$line" in
    *musl*) glibc="" ;;
    *) glibc="$(printf '%s' "$line" | grep -oE '[0-9]+\.[0-9]+' | tail -n1)" ;;
  esac
fi
[ -n "$glibc" ] || die "Clipp needs glibc 2.31+ (musl / Alpine isn't supported)."
ver_ge "$glibc" "2.31" || die "glibc $glibc is too old - Clipp needs 2.31 or newer."
note "glibc $glibc"

# ---- 3. pick a supported package manager -----------------------------------
step "Looking for a supported package manager..."
pm=""; file=""
if   command -v apt-get >/dev/null 2>&1; then pm=apt;    file="clipp-linux-$arch.deb"
elif command -v dnf     >/dev/null 2>&1; then pm=dnf;    file="clipp-linux-$arch.rpm"
elif command -v zypper  >/dev/null 2>&1; then pm=zypper; file="clipp-linux-$arch.rpm"
elif command -v pacman  >/dev/null 2>&1; then pm=pacman; file="clipp-linux-$arch.pkg.tar.zst"
fi

if [ -n "$pm" ]; then
  # ---- 4a. install the native package (resolves the Avahi dependency) ------
  note "found: $pm"
  pkg="$TMP/$file"
  step "Downloading $file ..."
  note "$BASE/$file"
  download "$BASE/$file" "$pkg"
  note "$(du -h "$pkg" | cut -f1) downloaded"

  step "Installing with $pm (this pulls in the Avahi dependency)..."
  # The packages are unsigned by design (integrity is via Sigstore attestation,
  # not repo GPG). Installing a *local* file needs no special flags: dnf defaults
  # to localpkg_gpgcheck=0, pacman to LocalFileSigLevel=Optional, and apt doesn't
  # GPG-check local .debs. zypper is the exception - in --non-interactive mode it
  # defaults the "install unsigned?" prompt to no, so it needs --allow-unsigned-rpm
  # (scoped to this file only; it does NOT relax checks on the dependencies).
  case "$pm" in
    apt)
      export DEBIAN_FRONTEND=noninteractive
      chmod a+rx "$TMP" 2>/dev/null || true
      chmod a+r "$pkg" 2>/dev/null || true
      $SUDO apt-get update -qq || true
      $SUDO apt-get install -y "$pkg"
      ;;
    dnf)
      $SUDO dnf install -y "$pkg"
      ;;
    zypper)
      $SUDO zypper --non-interactive install --allow-unsigned-rpm "$pkg"
      ;;
    pacman)
      $SUDO pacman -U --noconfirm "$pkg"
      ;;
  esac

  printf '\n'
  if command -v clipp >/dev/null 2>&1; then
    ok "Clipp installed: $(command -v clipp)"
  else
    ok "Clipp installed."
  fi
  note "Get started:  clipp key set   then   clipp copy / clipp paste"
  printf '\n'
else
  # ---- 4b. no supported PM: drop the static binary in the current directory -
  note "none found (apt / dnf / zypper / pacman)"
  bin="clipp-linux-$arch"
  step "Downloading the static binary ($arch)..."
  note "$BASE/$bin"
  download "$BASE/$bin" "./clipp"
  chmod +x ./clipp
  [ -n "${SUDO_USER:-}" ] && chown "$SUDO_USER" ./clipp 2>/dev/null || true

  printf '\n'
  ok "Saved ./clipp"
  note "It's in the current directory - move it onto your PATH, e.g.:"
  note "    sudo mv clipp /usr/local/bin/"
  printf '\n'
  note "Heads up: the packaged installs pull Avahi in automatically; this raw"
  note "binary does not. Clipp needs the Avahi client library at runtime, plus the"
  note "daemon running for peer discovery. Install them with your system's tools, e.g.:"
  note "    Debian/Ubuntu:  sudo apt install libavahi-client3 avahi-daemon"
  note "    Fedora/RHEL:    sudo dnf install avahi"
  note "then enable it:  sudo systemctl enable --now avahi-daemon"
  printf '\n'
  note "Get started:  ./clipp key set   then   ./clipp copy / ./clipp paste"
  printf '\n'
fi
