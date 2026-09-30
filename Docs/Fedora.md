# Fedora

## Dependencies

Full build (required + all optional features: Drive, PDF/SVG previews,
EPUB/archives, io_uring, native compress engines, video thumbnails,
GOA sign-in).

```bash
sudo dnf install gcc-c++ meson ninja-build cmake git pkgconf-pkg-config just \
  wayland-devel wayland-protocols-devel \
  freetype-devel fontconfig-devel \
  cairo-devel pango-devel \
  libxkbcommon-devel glib2-devel \
  sdbus-cpp-devel \
  libwebp-devel libjpeg-turbo-devel openssl-devel \
  libarchive-devel \
  libsoup3-devel json-glib-devel libsecret-devel \
  poppler-glib-devel librsvg2-devel \
  liburing-devel zlib-devel xz-devel libpng-devel \
  ffmpeg-free-devel \
  gtk4-devel libadwaita-devel gnome-online-accounts-devel \
  udisks2 xdg-utils
```

What pulls what (`meson.build` reference):

| pkg-config name | Fedora package | Notes |
|---|---|---|
| `wayland-client` | `wayland-devel` | also ships `wayland-scanner` |
| `wayland-protocols` | `wayland-protocols-devel` | `pkgdatadir` lookup in `src/archive_viewer/meson.build` |
| `cairo` / `pangocairo` | `cairo-devel` / `pango-devel` | |
| `xkbcommon` | `libxkbcommon-devel` | |
| `gio-2.0` | `glib2-devel` | |
| `libwebp` / `libjpeg` | `libwebp-devel` / `libjpeg-turbo-devel` | unconditional in `image_preview.cpp` |
| `libcrypto` | `openssl-devel` | |
| `sdbus-c++` | `sdbus-cpp-devel` | **required** (UDisks2 service) |
| `fontconfig` | `fontconfig-devel` (+ `freetype-devel`) | |
| `libarchive` | `libarchive-devel` | **required** — `src/archive_viewer` errors without it |
| `libsoup-3.0` / `json-glib-1.0` / `libsecret-1` | `libsoup3-devel` / `json-glib-devel` / `libsecret-devel` | Drive backend (`-DEH_HAVE_DRIVE` / `_KEYRING`), optional |
| `poppler-glib` | `poppler-glib-devel` | PDF thumbnails, optional |
| `librsvg-2.0` | `librsvg2-devel` | SVG thumbnails (NanoSVG fallback if absent), optional |
| `liburing` | `liburing-devel` | optional |
| `zlib` / `liblzma` | `zlib-devel` / `xz-devel` | native zip/gzip/xz/7z engines, optional |
| `libav*` + `libpng` | `ffmpeg-free-devel` + `libpng-devel` | `ffmpegthumbnailer` is built from `third_party/` via `cmake`. `ffmpeg-free-devel` is in the official repos and is enough. For the full codecs use RPMFusion's `ffmpeg-devel` instead: `sudo dnf install https://download1.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm && sudo dnf install ffmpeg-devel` |
| `gtk4` / `libadwaita-1` / `goa-1.0` + `goa-backend-1.0` | `gtk4-devel` / `libadwaita-devel` / `gnome-online-accounts-devel` | `horizon-goa-signin`, optional (skipped silently if absent) |
| build tools | `gcc-c++` `meson` `ninja-build` `cmake` `git` `pkgconf-pkg-config` | `ninja-build` is the Meson backend, `cmake` builds the vendored `ffmpegthumbnailer`, `git` fetches submodules |
| runtime helpers | `udisks2` (provides `udisksctl`) `xdg-utils` (provides `xdg-open`) | recommended |

> Or just run `./build.sh` — it detects Fedora/RHEL and installs all of the above
> (using `ffmpeg-free-devel` so no RPMFusion setup is required).

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
