#!/usr/bin/env bash
# Horizon File Manager — distro-aware build script.
# Detects Arch / Fedora (RHEL family) / Debian (Ubuntu family),
# installs full compile deps, builds release with --prefix=/usr,
# then optionally installs system-wide via pkexec.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-build-release}"
PREFIX="/usr"

SKIP_DEPS=0
AUTO_INSTALL=0

usage() {
  cat <<'EOF'
Usage: ./build.sh [--no-deps] [--yes] [--help]

  (no flags)   Install deps for your distro, build release to build-release/
               with --prefix=/usr, then ask to install system-wide via pkexec.
  --no-deps    Skip dependency installation (fail fast if tools are missing).
  --yes, -y    Install system-wide without asking (still via pkexec).
  --help, -h   Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-deps) SKIP_DEPS=1; shift ;;
    --yes|-y) AUTO_INSTALL=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

cd "$SCRIPT_DIR"

# ── distro detection ──────────────────────────────────────────────
DISTRO_ID=""
DISTRO_LIKE=""
if [[ -f /etc/os-release ]]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  DISTRO_ID="${ID:-}"
  DISTRO_LIKE="${ID_LIKE:-}"
fi
FAMILY=""
case "$DISTRO_ID $DISTRO_LIKE" in
  *arch*|*manjaro*|*cachyos*|*endeavour*|*garuda*)
    FAMILY="arch" ;;
  *fedora*|*rhel*|*centos*|*rocky*|*alma*|*nobara*|*ol*)
    FAMILY="fedora" ;;
  *debian*|*ubuntu*|*mint*|*pop*|*zorin*|*kali*|*elementary*)
    FAMILY="debian" ;;
  *)
    # ID_LIKE fallback (e.g. ID=pop ID_LIKE=ubuntu)
    case "$DISTRO_LIKE" in
      *arch*) FAMILY="arch" ;;
      *fedora*|*rhel*|*centos*) FAMILY="fedora" ;;
      *debian*|*ubuntu*) FAMILY="debian" ;;
    esac ;;
esac

if [[ "$SKIP_DEPS" -eq 0 && -z "$FAMILY" ]]; then
  echo "error: unsupported distro (ID='$DISTRO_ID' ID_LIKE='$DISTRO_LIKE')." >&2
  echo "Install deps manually — see Docs/Arch-Linux.md, Docs/Fedora.md," >&2
  echo "Docs/Debian-Ubuntu.md — then re-run with --no-deps." >&2
  exit 1
fi

# ── dependency installation ───────────────────────────────────────
install_deps_arch() {
  local pkgs=(
    base-devel meson ninja cmake git pkgconf gcc just
    wayland wayland-protocols
    freetype2 fontconfig
    cairo pango
    libxkbcommon glib2
    sdbus-cpp
    libwebp libjpeg-turbo openssl
    libarchive
    libsoup3 json-glib libsecret
    poppler-glib librsvg
    liburing zlib xz libpng
    ffmpeg
    gtk4 libadwaita gnome-online-accounts
    udisks2 xdg-utils
  )
  run_privileged pacman -S --needed --noconfirm "${pkgs[@]}"
}

install_deps_fedora() {
  local pkgs=(
    gcc-c++ meson ninja-build cmake git pkgconf-pkg-config just
    wayland-devel wayland-protocols-devel
    freetype-devel fontconfig-devel
    cairo-devel pango-devel
    libxkbcommon-devel glib2-devel
    sdbus-cpp-devel
    libwebp-devel libjpeg-turbo-devel openssl-devel
    libarchive-devel
    libsoup3-devel json-glib-devel libsecret-devel
    poppler-glib-devel librsvg2-devel
    liburing-devel zlib-devel xz-devel libpng-devel
    ffmpeg-free-devel
    gtk4-devel libadwaita-devel gnome-online-accounts-devel
    udisks2 xdg-utils
  )
  if ! run_privileged dnf install -y "${pkgs[@]}"; then
    echo "" >&2
    echo "dnf install failed. If only 'ffmpeg-free-devel' is missing, your" >&2
    echo "Fedora spin may need RPMFusion for the full 'ffmpeg-devel':" >&2
    echo "  sudo dnf install https://download1.rpmfusion.org/free/fedora/rpmfusion-free-release-\$(rpm -E %fedora).noarch.rpm" >&2
    echo "  sudo dnf install ffmpeg-devel" >&2
    exit 1
  fi
}

install_deps_debian() {
  local pkgs=(
    meson ninja-build cmake git pkg-config g++ just
    libwayland-dev wayland-protocols libwayland-bin
    libfreetype-dev libfontconfig-dev
    libcairo2-dev libpango1.0-dev
    libxkbcommon-dev libglib2.0-dev
    libsdbus-c++-dev
    libwebp-dev libjpeg-dev libssl-dev
    libarchive-dev
    libsoup-3.0-dev libjson-glib-dev libsecret-1-dev
    libpoppler-glib-dev librsvg2-dev
    liburing-dev zlib1g-dev liblzma-dev libpng-dev
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libavfilter-dev
    libgtk-4-dev libadwaita-1-dev libgoa-1.0-dev libgoa-backend-1.0-dev
    udisks2 xdg-utils
  )
  run_privileged apt update
  run_privileged apt install -y "${pkgs[@]}"
}

# Run a command as root: directly if already root, else via sudo, else pkexec.
run_privileged() {
  if [[ "$(id -u)" -eq 0 ]]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  elif command -v pkexec >/dev/null 2>&1; then
    pkexec "$@"
  else
    echo "error: need root (no sudo or pkexec found) to run: $*" >&2
    exit 1
  fi
}

if [[ "$SKIP_DEPS" -eq 0 ]]; then
  echo "==> Detected distro family: $FAMILY (ID='$DISTRO_ID')"
  echo "==> Installing build dependencies…"
  case "$FAMILY" in
    arch) install_deps_arch ;;
    fedora) install_deps_fedora ;;
    debian) install_deps_debian ;;
  esac
else
  echo "==> Skipping dependency installation (--no-deps)."
fi

# ── preflight ─────────────────────────────────────────────────────
missing=()
for t in meson ninja cmake git pkg-config; do
  command -v "$t" >/dev/null 2>&1 || missing+=("$t")
done
if ! command -v g++ >/dev/null 2>&1 && ! command -v c++ >/dev/null 2>&1; then
  missing+=("g++")
fi
if [[ "${#missing[@]}" -gt 0 ]]; then
  echo "error: missing build tools: ${missing[*]}" >&2
  echo "Re-run without --no-deps, or install them manually (see Docs/)." >&2
  exit 1
fi

# ── submodules (vendored third_party/) ────────────────────────────
if [[ -d .git ]]; then
  echo "==> Updating git submodules…"
  git submodule update --init --recursive
else
  echo "==> No .git directory — skipping submodule update."
  echo "    (tarball builds must already vendor third_party/.)"
fi

# ── configure + compile ───────────────────────────────────────────
echo "==> Configuring release build in $BUILD_DIR/ (prefix=$PREFIX)…"
# A previous sudo build may have left a root-owned dir behind — reclaim it
# so a normal user build works (build as user, install via pkexec).
if [[ -e "$BUILD_DIR" && ! -w "$BUILD_DIR" ]]; then
  echo "==> $BUILD_DIR/ is not writable (likely from a past sudo build) — removing with privileges…"
  run_privileged rm -rf "$BUILD_DIR"
fi
if [[ -f "$BUILD_DIR/build.ninja" ]]; then
  if ! meson setup --reconfigure "$BUILD_DIR" --prefix="$PREFIX" --buildtype=release -Dstrip=true; then
    echo "Reconfigure failed — wiping $BUILD_DIR/ and configuring fresh."
    rm -rf "$BUILD_DIR"
    meson setup "$BUILD_DIR" --prefix="$PREFIX" --buildtype=release -Dstrip=true
  fi
else
  # Stale dir without ninja file (or first run) → fresh configure.
  if [[ -d "$BUILD_DIR" && ! -f "$BUILD_DIR/build.ninja" ]]; then
    rm -rf "$BUILD_DIR"
  fi
  meson setup "$BUILD_DIR" --prefix="$PREFIX" --buildtype=release -Dstrip=true
fi

echo "==> Compiling…"
meson compile -C "$BUILD_DIR"
echo "==> Build OK: $BUILD_DIR/horizon-files"

# ── optional system-wide install via pkexec ───────────────────────
do_install=0
if [[ "$AUTO_INSTALL" -eq 1 ]]; then
  do_install=1
else
  answer=""
  # /dev/tty so the prompt works even when stdout is piped.
  if ! read -r -p "Install system-wide to $PREFIX? [y/N] " answer </dev/tty 2>/dev/null; then
    answer=""
  fi
  case "$answer" in
    [Yy]|[Yy][Ee][Ss]) do_install=1 ;;
    *) do_install=0 ;;
  esac
fi

if [[ "$do_install" -eq 1 ]]; then
  echo "==> Installing to $PREFIX (pkexec will ask for your password)…"
  if [[ "$(id -u)" -eq 0 ]]; then
    meson install -C "$BUILD_DIR"
  elif command -v pkexec >/dev/null 2>&1; then
    pkexec meson install -C "$BUILD_DIR"
  else
    echo "error: pkexec not found — cannot escalate for system install." >&2
    echo "Install polkit, or run manually: sudo meson install -C $BUILD_DIR" >&2
    exit 1
  fi
  echo "==> Installed. Run with: horizon-files"
else
  echo "Skipped system install. To install later:"
  echo "  pkexec meson install -C $BUILD_DIR"
  echo "Or run in place from a Wayland session:"
  echo "  ./$BUILD_DIR/horizon-files"
fi
