# Horizon File Manager

Wayland-native file manager (sidebar, tabs, grid/list, thumbnails, search, trash, file ops) for wlroots-style compositors. Single binary: **`horizon-files`**.

## Dependencies

You need a C++23 toolchain, **Meson**, **Ninja**, and development packages for at least:

Wayland (client), Cairo, Pango, xkbcommon, GLib/GIO, **sdbus-c++**, libwebp, libjpeg, OpenSSL, fontconfig/freetype — plus optional **poppler-glib** (PDF thumbnails), **libarchive** (EPUB/archive previews), **librsvg** (SVG thumbnails). Video thumbnails build **ffmpegthumbnailer** from source (needs libavcodec, libavformat, libavutil, libswscale).

Third-party code is vendored as git submodules (`third_party/`). After cloning:

```bash
git submodule update --init --recursive
```

If configure fails, install the missing `-dev` / `-devel` package Meson names in the error.

### Fedora

```bash
sudo dnf install meson g++ just \
  wayland-devel wayland-protocols-devel \
  freetype-devel fontconfig-devel \
  cairo-devel pango-devel \
  libxkbcommon-devel glib2-devel \
  sdbus-cpp-devel \
  libwebp-devel libjpeg-turbo-devel \
  openssl-devel cmake \
  ffmpeg-devel \
  poppler-glib-devel libarchive-devel librsvg2-devel
```

### Arch Linux

```bash
sudo pacman -S meson gcc just \
  wayland wayland-protocols \
  freetype2 fontconfig \
  cairo pango \
  libxkbcommon glib2 \
  sdbus-cpp \
  libwebp libjpeg-turbo openssl cmake \
  ffmpeg \
  poppler-glib libarchive librsvg
```

### Debian / Ubuntu

```bash
sudo apt install meson g++ just \
  libwayland-dev wayland-protocols \
  libfreetype-dev libfontconfig-dev \
  libcairo2-dev libpango1.0-dev \
  libxkbcommon-dev libglib2.0-dev \
  libsdbus-c++-dev \
  libwebp-dev libjpeg-dev libssl-dev cmake \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libpoppler-glib-dev libarchive-dev librsvg2-dev
```

## Compile

```bash
git submodule update --init --recursive
meson setup build
meson compile -C build
```

Release build:

```bash
meson setup build-release --buildtype=release -Dstrip=true
meson compile -C build-release
```

### With `just`

- **`just build`** — creates `build-debug/` if needed, then compiles a debug build.
- **`just build-release`** — creates `build-release/` if needed, then compiles a release build.
- **`just build-asan`** — AddressSanitizer + UndefinedBehaviourSanitizer build.
- **`just run`** — full clean + release build + run.
- **`just install`** — release build + install to `/usr`.

## Install to `/usr`

```bash
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build
```

## Run

From a Wayland session (e.g. Hyprland):

```bash
horizon-files
```
