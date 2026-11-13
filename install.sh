#!/bin/sh
# taskmgr one-line installer
#
#   curl -fsSL https://raw.githubusercontent.com/Artyuiui/taskmgr/main/install.sh | sh
#
# 1. Downloads the prebuilt binary for your CPU from the latest GitHub release
#    (x86_64 / aarch64, glibc-based distros) and verifies its SHA-256.
# 2. If no prebuilt binary fits (musl/Alpine, armv7, riscv64, old glibc ...),
#    installs the compiler + ncurses headers with your package manager and
#    builds from source instead.
#
# Environment overrides:
#   TASKMGR_INSTALL_DIR=/some/bin   install location (default /usr/local/bin, or ~/.local/bin without root)
#   TASKMGR_FROM_SOURCE=1           skip the prebuilt binary and always build
#   TASKMGR_VERSION=v0.1.0          install a specific release instead of the latest
set -eu

REPO="Artyuiui/taskmgr"
BIN="taskmgr"

say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

[ "$(uname -s)" = "Linux" ] || die "taskmgr reads /proc and /sys, so it only runs on Linux."

ARCH=$(uname -m)
TMP=$(mktemp -d 2>/dev/null || mktemp -d -t taskmgr)
trap 'rm -rf "$TMP"' EXIT INT TERM

# ---------------------------------------------------------------- helpers
fetch() {   # fetch URL DEST
    if have curl; then curl -fsSL --retry 3 -o "$2" "$1"
    elif have wget; then wget -qO "$2" "$1"
    else die "need curl or wget"; fi
}

SUDO=""
pick_install_dir() {
    if [ -n "${TASKMGR_INSTALL_DIR:-}" ]; then
        DEST="$TASKMGR_INSTALL_DIR"
    elif [ -w /usr/local/bin ]; then
        DEST=/usr/local/bin
    elif have sudo; then
        DEST=/usr/local/bin; SUDO="sudo"
    else
        DEST="$HOME/.local/bin"
    fi
}

install_bin() {   # install_bin FILE
    pick_install_dir
    $SUDO mkdir -p "$DEST"
    $SUDO install -m 755 "$1" "$DEST/$BIN"
    say "installed $DEST/$BIN"
    case ":$PATH:" in
        *":$DEST:"*) ;;
        *) warn "$DEST is not in your PATH. Add this to your shell profile:  export PATH=\"$DEST:\$PATH\"" ;;
    esac
    "$DEST/$BIN" -v 2>/dev/null || true
    say "run it with:  $BIN"
}

# ---------------------------------------------------------------- prebuilt
try_prebuilt() {
    [ -z "${TASKMGR_FROM_SOURCE:-}" ] || return 1
    case "$ARCH" in
        x86_64|aarch64) ;;
        *) say "no prebuilt binary for $ARCH, building from source"; return 1 ;;
    esac
    # prebuilt binaries are glibc-linked; musl systems (Alpine, Void-musl) build from source
    if ! ls /lib/ld-linux* /lib64/ld-linux* /lib/*/ld-linux* >/dev/null 2>&1; then
        say "no glibc dynamic loader found (musl?), building from source"; return 1
    fi

    if [ -n "${TASKMGR_VERSION:-}" ]; then
        BASE="https://github.com/$REPO/releases/download/$TASKMGR_VERSION"
    else
        BASE="https://github.com/$REPO/releases/latest/download"
    fi
    ASSET="$BIN-linux-$ARCH"
    say "downloading $ASSET"
    fetch "$BASE/$ASSET" "$TMP/$ASSET" || { warn "download failed (no release yet?), building from source"; return 1; }
    fetch "$BASE/SHA256SUMS" "$TMP/SHA256SUMS" || { warn "checksum file missing, building from source"; return 1; }

    expected=$(grep " $ASSET\$" "$TMP/SHA256SUMS" | awk '{print $1}')
    if have sha256sum; then actual=$(sha256sum "$TMP/$ASSET" | awk '{print $1}')
    elif have shasum; then actual=$(shasum -a 256 "$TMP/$ASSET" | awk '{print $1}')
    else warn "no sha256sum tool, skipping verification"; actual="$expected"; fi
    [ -n "$expected" ] && [ "$expected" = "$actual" ] || die "checksum mismatch for $ASSET"
    say "checksum OK"

    chmod +x "$TMP/$ASSET"
    # make sure the shared libs it needs (libncursesw.so.6 / libtinfo.so.6) exist here
    if ! "$TMP/$ASSET" -v >/dev/null 2>&1; then
        warn "prebuilt binary does not run on this system (missing libncursesw6 or old glibc), building from source"
        return 1
    fi
    install_bin "$TMP/$ASSET"
}

# ---------------------------------------------------------------- from source
install_deps() {
    PM_SUDO=""; [ "$(id -u)" = 0 ] || PM_SUDO="sudo"
    if have apt-get; then $PM_SUDO apt-get update -qq && $PM_SUDO apt-get install -y -qq build-essential libncursesw5-dev pkg-config
    elif have dnf; then $PM_SUDO dnf install -y gcc make ncurses-devel pkgconf-pkg-config
    elif have yum; then $PM_SUDO yum install -y gcc make ncurses-devel pkgconfig
    elif have pacman; then $PM_SUDO pacman -S --needed --noconfirm gcc make ncurses pkgconf
    elif have apk; then $PM_SUDO apk add --no-cache build-base ncurses-dev pkgconf
    elif have zypper; then $PM_SUDO zypper --non-interactive install gcc make ncurses-devel pkg-config
    elif have xbps-install; then $PM_SUDO xbps-install -Sy gcc make ncurses-devel pkg-config
    elif have emerge; then $PM_SUDO emerge --ask=n sys-libs/ncurses
    else warn "unknown package manager: make sure a C compiler, make and the ncurses(w) headers are installed"; fi
}

build_from_source() {
    if ! have cc && ! have gcc && ! have clang || ! have make || ! have pkg-config; then
        say "installing build dependencies (compiler, make, ncurses headers)"
        install_deps
    elif ! pkg-config --exists ncursesw 2>/dev/null && ! pkg-config --exists ncurses 2>/dev/null && [ ! -f /usr/include/ncurses.h ]; then
        say "installing ncurses headers"
        install_deps
    fi
    REF="${TASKMGR_VERSION:-main}"
    say "downloading source ($REF)"
    fetch "https://github.com/$REPO/archive/$REF.tar.gz" "$TMP/src.tar.gz" ||
        fetch "https://github.com/$REPO/archive/refs/heads/$REF.tar.gz" "$TMP/src.tar.gz" ||
        die "could not download the source archive"
    mkdir -p "$TMP/src" && tar -xzf "$TMP/src.tar.gz" -C "$TMP/src" --strip-components=1
    say "building"
    make -C "$TMP/src" release VERSION="$REF" >/dev/null
    install_bin "$TMP/src/$BIN"
}

say "taskmgr installer  (Linux $ARCH)"
try_prebuilt || build_from_source
