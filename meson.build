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
  '-include', 'platform/common/log/mangowm_logger.hpp',
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

# Optional: poppler (PDF preview), libarchive, librsvg, io_uring
dep_poppler   = dependency('poppler-glib', required: false)
dep_archive   = dependency('libarchive', required: false)
dep_librsvg   = dependency('librsvg-2.0', required: false)
dep_uring     = dependency('liburing', required: false)

if dep_poppler.found()
  cpp_args += ['-DEH_HAVE_POPPLER']
endif
if dep_uring.found()
  cpp_args += ['-DEH_HAVE_IO_URING']
endif
if dep_archive.found()
  cpp_args += ['-DEH_HAVE_LIBARCHIVE']
endif
if dep_librsvg.found()
  cpp_args += ['-DEH_HAVE_RSVG']
endif

if dep_jpeg.found()
  cpp_args += ['-DEH_HAVE_LIBJPEG']
endif

# ── Horizon Colors: Google material-color-utilities (C++, Apache-2.0) ────────
# Vendored material-color-utilities references Abseil for exactly two things:
# `absl::flat_hash_map` (wsmeans quantizer) and `StrCat(absl::Hex(x))` (utils).
# Instead of linking any Abseil build — system or vendored, either way the
# binary ends up coupled to a libabsl SONAME that differs per distro and broke
# every packaged build — compile-time shims provide those two names over the
# C++ standard library. The submodule stays pristine; no libabsl at runtime.
mcu_absl_shim_inc = include_directories('third_party/absl-shim')

material_colors_sources = files(
  'third_party/material-color-utilities/cpp/blend/blend.cc',
  'third_party/material-color-utilities/cpp/cam/cam.cc',
  'third_party/material-color-utilities/cpp/cam/hct.cc',
  'third_party/material-color-utilities/cpp/cam/hct_solver.cc',
  'third_party/material-color-utilities/cpp/cam/viewing_conditions.cc',
  'third_party/material-color-utilities/cpp/contrast/contrast.cc',
  'third_party/material-color-utilities/cpp/dislike/dislike.cc',
  'third_party/material-color-utilities/cpp/dynamiccolor/dynamic_color.cc',
  'third_party/material-color-utilities/cpp/dynamiccolor/dynamic_scheme.cc',
  'third_party/material-color-utilities/cpp/dynamiccolor/material_dynamic_colors.cc',
  'third_party/material-color-utilities/cpp/palettes/tones.cc',
  'third_party/material-color-utilities/cpp/quantize/celebi.cc',
  'third_party/material-color-utilities/cpp/quantize/lab.cc',
  'third_party/material-color-utilities/cpp/quantize/wsmeans.cc',
  'third_party/material-color-utilities/cpp/quantize/wu.cc',
  'third_party/material-color-utilities/cpp/scheme/scheme_content.cc',
  'third_party/material-color-utilities/cpp/scheme/scheme_expressive.cc',
  'third_party/material-color-utilities/cpp/scheme/scheme_fidelity.cc',
  'third_party/material-color-utilities/cpp/scheme/scheme_fruit_salad.cc',
  'third_party/material-color-utilities/cpp/scheme/scheme_monochrome.cc',
  'third_party/material-color-utilities/cpp/scheme/scheme_neutral.cc',
  'third_party/material-color-utilities/cpp/scheme/scheme_rainbow.cc',
  'third_party/material-color-utilities/cpp/scheme/scheme_tonal_spot.cc',
  'third_party/material-color-utilities/cpp/scheme/scheme_vibrant.cc',
  'third_party/material-color-utilities/cpp/score/score.cc',
  'third_party/material-color-utilities/cpp/temperature/temperature_cache.cc',
  'third_party/material-color-utilities/cpp/utils/utils.cc',
)

material_colors_inc = include_directories('third_party/material-color-utilities')

eh_material_colors_lib = static_library(
  'eh_material_colors',
  sources: material_colors_sources,
  include_directories: [material_colors_inc, mcu_absl_shim_inc],
  cpp_args: ['-Wno-unused-parameter', '-Wno-extra-semi', '-Wno-pedantic', '-Wno-extra-semi-braces'],
  override_options: ['optimization=2', 'werror=false'],
  cpp_pch: 'src/platform/common/palette/pch_material_colors.hpp',
  pic: true,
)

eh_material_colors_dep = declare_dependency(
  link_with: eh_material_colors_lib,
  include_directories: [material_colors_inc, mcu_absl_shim_inc],
)

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
  'src/app/file_browser/embed/embed.cpp',
  'src/app/file_browser/features/compress.cpp',
  'src/app/file_browser/features/compare.cpp',
  'src/app/file_browser/features/desktop_icon_parser.cpp',
  'src/app/file_browser/features/drag.cpp',
  'src/app/file_browser/features/epub_preview.cpp',
  'src/app/file_browser/features/filetype.cpp',
  'src/app/file_browser/features/scan.cpp',
  'src/app/file_browser/features/sidebar.cpp',
  'src/app/file_browser/features/preview_popup.cpp',
  'src/app/file_browser/features/image_preview.cpp',
  'src/app/file_browser/features/menu.cpp',
  'src/app/file_browser/features/nav.cpp',
  'src/app/file_browser/features/pdf_preview.cpp',
  'src/app/file_browser/features/progress.cpp',
  'src/app/file_browser/features/query_match.cpp',
  'src/app/file_browser/features/dirprops.cpp',
  'src/app/file_browser/features/recursive_search_worker.cpp',
  'src/app/file_browser/features/selection.cpp',
  'src/app/file_browser/features/svg_preview.cpp',
  'src/app/file_browser/features/tab_history.cpp',
  'src/app/file_browser/features/tags.cpp',
  'src/app/file_browser/features/view_zoom.cpp',
  'src/app/file_browser/features/thumbnail_cache.cpp',
  'src/app/file_browser/features/video_preview.cpp',
  'src/app/file_browser/features/video_worker.cpp',
  'src/app/file_browser/features/thumb_pool.cpp',
  'src/app/file_browser/features/dir_stats.cpp',
  'src/app/file_browser/features/dir_watch.cpp',
  'src/app/file_browser/features/terminal.cpp',
  'src/app/file_browser/ui/computer_view.cpp',
  'src/app/file_browser/ui/rename_ui.cpp',
  'src/app/file_browser/ui/pixblit.cpp',
  'src/app/file_browser/ui/draw.cpp',
  'src/app/file_browser/ui/layout.cpp',
  'src/app/file_browser/ui/draw_helpers.cpp',
  'src/app/file_browser/ui/draw_file_icons.cpp',
  'src/app/file_browser/ui/draw_thumbnails.cpp',
  'src/app/file_browser/ui/draw_topbar.cpp',
  'src/app/file_browser/ui/draw_dialogs.cpp',
  'src/app/file_browser/input/events.cpp',
  'src/app/file_browser/input/click.cpp',
  'src/app/file_browser/input/keyboard.cpp',
  'src/app/file_browser/input/pointer.cpp',
  'src/app/file_browser/input/release.cpp',
  'src/app/file_browser/input/scroll.cpp',
  'src/app/file_browser/app.cpp',

  # Wayland core
  'src/wayland/core/connection.cpp',
  'src/wayland/core/seat.cpp',
  'src/wayland/core/memfd.cpp',
  'src/wayland/buffer/shm_buffer.cpp',
  'src/wayland/buffer/cairo_cpu_buffer.cpp',
  'src/wayland/buffer/single_pixel_buffer.cpp',
  'src/wayland/input/clipboard.cpp',

  # Surface extensions (only the file used by the file browser)
  'src/wayland/surface/surface_extensions.cpp',
  'src/wayland/surface/tearing_control.cpp',

  # Configuration (standalone)
  'src/config/shell_config.cpp',

  # Desktop shell
  'src/platform/desktop/entries/desktop_xdg_ops.cpp',
  'src/platform/common/asset/asset_loader.cpp',
  'src/platform/common/glyph/bundled_assets.cpp',
  'src/platform/common/glyph/material_glyph.cpp',
  'src/platform/common/icon_cache/icon_cache.cpp',
  'src/platform/common/fs/shell_paths.cpp',
  'src/platform/common/fs/string_util.cpp',
  'src/platform/common/palette/matugen_palette.cpp',
  'src/platform/common/palette/horizon_colors.cpp',

  # Bootstrap thread
  'src/base/thread/thread_dispatch.cpp',
  'src/base/thread/thread_pool.cpp',

  # Services
  'src/services/udisks2/udisks2_drive_service.cpp',

  # Dialog
  'src/dialog/dialog_base.cpp',
  'src/dialog/file_chooser_dialog.cpp',

  # Desktop list
  'src/platform/widgets/app_drawer/list/desktop_list.cpp',

  # Standalone main
  'src/app/file_browser/standalone_main.cpp',

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

# ── Archive viewer sub-directory ──────────────────────────────────────
subdir('src/archive_viewer')

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
    dep_uring,
    eh_material_colors_dep,
  ],
  install: true,
)

# ── Desktop entry ────────────────────────────────────────────────────
install_data('src/horizon-files.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'))

# ── Icon ─────────────────────────────────────────────────────────────
install_data(
  join_paths(meson.project_source_root(), 'assets', 'icons', 'Horizon-Files.svg'),
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps'),
  rename: 'horizon-filesv2.svg',
)

# ── UI icon assets (top-bar buttons, folder glyphs, etc.) ───────────
# Installed to <prefix>/share/event-horizon/assets/UI — one of the paths
# eh::shell::asset::find_asset() searches at runtime.
install_subdir(
  join_paths(meson.project_source_root(), 'assets', 'UI'),
  install_dir: join_paths(get_option('datadir'), 'event-horizon', 'assets'),
)


# ── Tests ─────────────────────────────────────────────────────────────
test_cpp_args = cpp_args

icon_cache_test = executable(
  'test_icon_cache',
  'tests/test_icon_cache.cpp',
  'src/platform/common/icon_cache/icon_cache.cpp',
  include_directories: inc,
  cpp_args: test_cpp_args,
  dependencies: [dep_cairo, dependency('threads')],
)

test('icon_cache', icon_cache_test)
