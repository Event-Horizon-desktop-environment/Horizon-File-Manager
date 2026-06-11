project(
  'horizon-files',
  'c', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
    'buildtype=release',
    'b_ndebug=if-release',
  ]
)

# ── C++ flags ─────────────────────────────────────────────────────────
cpp_args = [
  '-Werror',
  '-Wno-unused-parameter',
  '-Wno-unused-variable',
  '-Wno-maybe-uninitialized',
  '-Wno-deprecated-literal-operator',
  # Forced includes to replace the original project's PCH
  '-include', 'desktop_shell/common/log/mangowm_logger.hpp',
]

# ── Dependencies (system) ─────────────────────────────────────────────
dep_wayland    = dependency('wayland-client')
dep_cairo      = dependency('cairo')
dep_pango      = dependency('pangocairo')
dep_xkbcommon  = dependency('xkbcommon')
dep_glib       = dependency('gio-2.0')
dep_webp       = dependency('libwebp')
dep_jpeg       = dependency('libjpeg')
dep_openssl    = dependency('libcrypto')
dep_sdbus      = dependency('sdbus-c++')
dep_fontconfig = dependency('fontconfig')

# Optional: poppler (PDF preview), libarchive, librsvg
dep_poppler   = dependency('poppler-glib', required: false)
dep_archive   = dependency('libarchive', required: false)
dep_librsvg   = dependency('librsvg-2.0', required: false)

if dep_poppler.found()
  cpp_args += ['-DEH_HAVE_POPPLER']
endif
if dep_archive.found()
  cpp_args += ['-DEH_HAVE_LIBARCHIVE']
endif
if dep_librsvg.found()
  cpp_args += ['-DEH_HAVE_RSVG']
endif

# ── Include paths ─────────────────────────────────────────────────────
inc = include_directories(
  'src',
  'protocol',
  'third_party',
  'third_party/tomlplusplus/include',
)

# ── Wayland protocol headers ─────────────────────────────────────────
# Already generated and copied to protocol/ — no wayland-scanner step needed.

# ── Sources ───────────────────────────────────────────────────────────
sources = files(
  # File browser core
  'src/ux/file_browser/embed/embed.cpp',
  'src/ux/file_browser/features/compress.cpp',
  'src/ux/file_browser/features/drag.cpp',
  'src/ux/file_browser/features/epub_preview.cpp',
  'src/ux/file_browser/features/image_preview.cpp',
  'src/ux/file_browser/features/menu.cpp',
  'src/ux/file_browser/features/nav.cpp',
  'src/ux/file_browser/features/pdf_preview.cpp',
  'src/ux/file_browser/features/progress.cpp',
  'src/ux/file_browser/features/recursive_search_worker.cpp',
  'src/ux/file_browser/features/svg_preview.cpp',
  'src/ux/file_browser/features/thumbnail_cache.cpp',
  'src/ux/file_browser/features/video_preview.cpp',
  'src/ux/file_browser/features/video_worker.cpp',
  'src/ux/file_browser/features/terminal.cpp',
  'src/ux/file_browser/ui/computer_view.cpp',
  'src/ux/file_browser/ui/rename_ui.cpp',
  'src/ux/file_browser/ui/draw.cpp',
  'src/ux/file_browser/input/events.cpp',
  'src/ux/file_browser/app.cpp',

  # Wayland core
  'src/wl/core/connection.cpp',
  'src/wl/core/seat.cpp',
  'src/wl/core/memfd.cpp',
  'src/wl/buffer/shm_buffer.cpp',
  'src/wl/buffer/cairo_cpu_buffer.cpp',
  'src/wl/buffer/single_pixel_buffer.cpp',
  'src/wl/input/clipboard.cpp',

  # Surface extensions (only the file used by the file browser)
  'src/wl/surface/surface_extensions.cpp',
  'src/wl/surface/tearing_control.cpp',

  # Configuration (standalone)
  'src/configuration/shell_config.cpp',

  # Desktop shell
  'src/desktop_shell/desktop/entries/desktop_xdg_ops.cpp',
  'src/desktop_shell/common/asset/asset_loader.cpp',
  'src/desktop_shell/common/glyph/bundled_assets.cpp',
  'src/desktop_shell/common/glyph/material_glyph.cpp',
  'src/desktop_shell/common/icon_cache/icon_cache.cpp',
  'src/desktop_shell/common/fs/shell_paths.cpp',
  'src/desktop_shell/common/fs/string_util.cpp',
  'src/desktop_shell/common/palette/matugen_palette.cpp',

  # Bootstrap thread
  'src/bootstrap/thread/thread_dispatch.cpp',
  'src/bootstrap/thread/thread_pool.cpp',

  # Services (standalone stubs)
  'src/services/udisks2/udisks2_drive_service.cpp',

  # Dialog (standalone stubs)
  'src/dialog/dialog_base.cpp',
  'src/dialog/file_chooser_dialog.cpp',

  # Wallpaper (standalone stubs)
  'src/wallpaper/thumbnail/wallpaper_thumbnail.cpp',

  # Backend interfaces (standalone stubs)
  'src/backends/interfaces/compositor_ipc.cpp',

  # Spotlight search stub
  'src/desktop_shell/spotlight/search/spotlight_search.cpp',

  # Desktop list stub
  'src/desktop_shell/widgets/app_drawer/list/desktop_list.cpp',

  # Standalone main
  'src/ux/file_browser/standalone_main.cpp',

  # Wayland protocol code (generated .c files)
  'protocol/xdg-shell-client-protocol.c',
  'protocol/viewporter-client-protocol.c',
  'protocol/fractional-scale-v1-client-protocol.c',
  'protocol/tearing-control-v1-client-protocol.c',
  'protocol/ext-data-control-v1-client-protocol.c',
  'protocol/wlr-data-control-unstable-v1-client-protocol.c',
)

# ── Build ffmpegthumbnailer from source ──────────────────────────────
ffmpegthumb_build = meson.current_build_dir() / 'ffmpegthumbnailer'
ffmpegthumb_result = run_command(
  'cmake', '-S', 'third_party/ffmpegthumbnailer',
  '-B', ffmpegthumb_build,
  '-DCMAKE_BUILD_TYPE=Release',
  '-DENABLE_GIO=OFF',
  '-DENABLE_EXECUTABLE=ON',
  '-DENABLE_STATIC=OFF',
  check: false,
)
if ffmpegthumb_result.returncode() == 0
  run_command(
    'cmake', '--build', ffmpegthumb_build, '--', '-j4',
    check: false,
  )
  ffmpegthumb_bin = ffmpegthumb_build / 'ffmpegthumbnailer'
  cpp_args += ['-DFFMPEGTHUMBNAILER_BIN="@0@"'.format(ffmpegthumb_bin)]
  message('ffmpegthumbnailer: built from submodule')
else
  message('ffmpegthumbnailer: NOT built (cmake failed) — video thumbnails disabled')
endif

# ── Executable ────────────────────────────────────────────────────────
executable(
  'horizon-files',
  sources,
  include_directories: inc,
  cpp_args: cpp_args,
  dependencies: [
    dep_wayland,
    dep_cairo,
    dep_pango,
    dep_xkbcommon,
    dep_glib,
    dep_webp,
    dep_jpeg,
    dep_openssl,
    dep_poppler,
    dep_archive,
    dep_librsvg,
    dep_sdbus,
    dep_fontconfig,
  ],
  install: true,
)
