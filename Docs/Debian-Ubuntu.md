# Debian / Ubuntu

## Dependencies

Full build (required + all optional features: Drive, PDF/SVG previews,
EPUB/archives, io_uring, native compress engines, video thumbnails,
GOA sign-in). Tested on Debian 13 (trixie) and Ubuntu 24.04+.

```bash
sudo apt update
sudo apt install meson ninja-build cmake git pkg-config g++ just \
  libwayland-dev wayland-protocols libwayland-bin \
  libfreetype-dev libfontconfig-dev \
  libcairo2-dev libpango1.0-dev \
  libxkbcommon-dev libglib2.0-dev \
  libsdbus-c++-dev \
  libwebp-dev libjpeg-dev libssl-dev \
  libarchive-dev \
  libsoup-3.0-dev libjson-glib-dev libsecret-1-dev \
  libpoppler-glib-dev librsvg2-dev \
  liburing-dev zlib1g-dev liblzma-dev libpng-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libavfilter-dev \
  libgtk-4-dev libadwaita-1-dev libgoa-1.0-dev libgoa-backend-1.0-dev \
  udisks2 xdg-utils
```

What pulls what (`meson.build` reference):

| pkg-config name | Debian/Ubuntu package | Notes |
|---|---|---|
| `wayland-client` | `libwayland-dev` | |
| `wayland-protocols` | `wayland-protocols` | `pkgdatadir` lookup in `src/archive_viewer/meson.build` |
| `wayland-scanner` (binary) | `libwayland-bin` | needed by `src/archive_viewer` custom targets; **not** pulled by `-dev` alone |
| `cairo` / `pangocairo` | `libcairo2-dev` / `libpango1.0-dev` | |
| `xkbcommon` | `libxkbcommon-dev` | |
| `gio-2.0` | `libglib2.0-dev` | |
| `libwebp` / `libjpeg` | `libwebp-dev` / `libjpeg-dev` | unconditional in `image_preview.cpp` |
| `libcrypto` | `libssl-dev` | |
| `sdbus-c++` | `libsdbus-c++-dev` | **required** (UDisks2 service) |
| `fontconfig` | `libfontconfig-dev` (+ `libfreetype-dev`) | transitional name on older releases is `libfontconfig1-dev` / `libfreetype6-dev` |
| `libarchive` | `libarchive-dev` | **required** — `src/archive_viewer` errors without it |
| `libsoup-3.0` / `json-glib-1.0` / `libsecret-1` | `libsoup-3.0-dev` / `libjson-glib-dev` / `libsecret-1-dev` | Drive backend (`-DEH_HAVE_DRIVE` / `_KEYRING`), optional |
| `poppler-glib` | `libpoppler-glib-dev` | PDF thumbnails, optional |
| `librsvg-2.0` | `librsvg2-dev` | SVG thumbnails (NanoSVG fallback if absent), optional |
| `liburing` | `liburing-dev` | optional |
| `zlib` / `liblzma` | `zlib1g-dev` / `liblzma-dev` | native zip/gzip/xz/7z engines, optional |
| `libav*` + `libpng` | `libavcodec-dev` `libavformat-dev` `libavutil-dev` `libswscale-dev` `libavfilter-dev` + `libpng-dev` | `ffmpegthumbnailer` is built from `third_party/` via `cmake` (`FindFFmpeg` needs **avfilter** too) |
| `gtk4` / `libadwaita-1` / `goa-1.0` + `goa-backend-1.0` | `libgtk-4-dev` / `libadwaita-1-dev` / `libgoa-1.0-dev` + `libgoa-backend-1.0-dev` | `horizon-goa-signin`, optional (skipped silently if absent) |
| build tools | `g++` (`build-essential`) `meson` `ninja-build` `cmake` `git` `pkg-config` | `ninja-build` is the Meson backend, `cmake` builds the vendored `ffmpegthumbnailer`, `git` fetches submodules |
| runtime helpers | `udisks2` (provides `udisksctl`) `xdg-utils` (provides `xdg-open`) | recommended |

> Or just run `./build.sh` — it detects Debian/Ubuntu and installs all of the above.

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
- **`just install`** — release build + install to `/usr`.
