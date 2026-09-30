# Arch Linux

## Dependencies

Full build (required + all optional features: Drive, PDF/SVG previews,
EPUB/archives, io_uring, native compress engines, video thumbnails,
GOA sign-in). Runtime helpers (`udisks2`, `xdg-utils`) are recommended.

```bash
sudo pacman -S --needed \
  base-devel meson ninja cmake git pkgconf gcc just \
  wayland wayland-protocols \
  freetype2 fontconfig \
  cairo pango \
  libxkbcommon glib2 \
  sdbus-cpp \
  libwebp libjpeg-turbo openssl \
  libarchive \
  libsoup3 json-glib libsecret \
  poppler-glib librsvg \
  liburing zlib xz libpng \
  ffmpeg \
  gtk4 libadwaita gnome-online-accounts \
  udisks2 xdg-utils
```

What pulls what (`meson.build` reference):

| pkg-config name | Arch package | Notes |
|---|---|---|
| `wayland-client` | `wayland` | also ships `wayland-scanner` |
| `wayland-protocols` | `wayland-protocols` | `pkgdatadir` lookup in `src/archive_viewer/meson.build` |
| `cairo` / `pangocairo` | `cairo` / `pango` | |
| `xkbcommon` | `libxkbcommon` | |
| `gio-2.0` | `glib2` | |
| `libwebp` / `libjpeg` | `libwebp` / `libjpeg-turbo` | unconditional in `image_preview.cpp` |
| `libcrypto` | `openssl` | |
| `sdbus-c++` | `sdbus-cpp` | **required** (UDisks2 service) |
| `fontconfig` | `fontconfig` (+ `freetype2`) | |
| `libarchive` | `libarchive` | **required** — `src/archive_viewer` errors without it |
| `libsoup-3.0` / `json-glib-1.0` / `libsecret-1` | `libsoup3` / `json-glib` / `libsecret` | Drive backend (`-DEH_HAVE_DRIVE` / `_KEYRING`), optional |
| `poppler-glib` | `poppler-glib` | PDF thumbnails, optional |
| `librsvg-2.0` | `librsvg` | SVG thumbnails (NanoSVG fallback if absent), optional |
| `liburing` | `liburing` | optional |
| `zlib` / `liblzma` | `zlib` / `xz` | native zip/gzip/xz/7z engines, optional |
| `libavformat` etc. + `libpng` | `ffmpeg` / `libpng` | `ffmpegthumbnailer` is built from `third_party/` via `cmake`; on Arch `ffmpeg` already ships headers |
| `gtk4` / `libadwaita-1` / `goa-1.0` + `goa-backend-1.0` | `gtk4` / `libadwaita` / `gnome-online-accounts` | `horizon-goa-signin`, optional (skipped silently if absent) |
| build tools | `base-devel` `meson` `ninja` `cmake` `git` `pkgconf` `gcc` | `ninja` is the Meson backend, `cmake` builds the vendored `ffmpegthumbnailer`, `git` fetches submodules |

> Or just run `./build.sh` — it detects Arch and installs all of the above.

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
