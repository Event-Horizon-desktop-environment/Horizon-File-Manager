# horizon-thumbnailer: single-shot decoder helper (see main.cpp).
# Same pure decoders as the in-process path so poppler/rsvg/archive/
# jpeg/webp live outside horizon-files RSS on a miss.
executable('horizon-thumbnailer',
  files(
    'main.cpp',
    'stb_impl.cpp',
    '../../app/file_browser/features/preview/preview_types.cpp',
    '../../app/file_browser/features/preview/image_preview.cpp',
    '../../app/file_browser/features/preview/svg_preview.cpp',
    '../../app/file_browser/features/preview/pdf_preview.cpp',
    '../../app/file_browser/features/preview/epub_preview.cpp',
    '../../app/file_browser/features/thumbnails/thumbnail_cache.cpp',
  ),
  include_directories: inc,
  cpp_args: cpp_args,
  dependencies: [
    dep_cairo,
    dep_webp,
    dep_jpeg,
    dep_openssl,
    dep_poppler,
    dep_archive,
    dep_librsvg,
  ],
  install: true,
)
