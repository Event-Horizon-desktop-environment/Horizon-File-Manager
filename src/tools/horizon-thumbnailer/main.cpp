// horizon-thumbnailer — single-shot out-of-process thumbnail decoder.
//
// Usage: horizon-thumbnailer <source-path> <max-px> <out.png>
// Exit: 0 = decoded, 1 = usage/unsupported type, 2 = decode failed.
//
// This is the split half of thumb_decode_sync() (see
// src/app/file_browser/ui/draw_thumbnails.cpp): the same pure decoders run
// here so poppler/librsvg/libarchive/jpeg/webp + ffmpegthumbnailer live
// outside horizon-files RSS. The main process keeps its 16 MB LRU and disk
// cache; it spawns this helper on a miss (EH_THUMB_HELPER=1) with fallback
// to in-process decode when the helper is missing.
#include <cairo/cairo.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/file_browser/features/preview/epub_preview.hpp"
#include "app/file_browser/features/preview/image_preview.hpp"
#include "app/file_browser/features/preview/pdf_preview.hpp"
#include "app/file_browser/features/preview/svg_preview.hpp"
#include "app/file_browser/features/preview/video_preview.hpp"

namespace {
// Mirror of is_video_extension() in
// src/app/file_browser/features/preview/video_preview.cpp (kept local so
// the helper does not link VideoThumbWorker/AppState).
bool is_video_path(const std::string& path) {
  auto ends_ci = [](const std::string& p, const char* suf) {
    size_t sl = std::strlen(suf);
    if (p.size() < sl) return false;
    for (size_t i = 0; i < sl; ++i) {
      char a = p[p.size() - sl + i];
      char b = suf[i];
      if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
      if (a != b) return false;
    }
    return true;
  };
  return ends_ci(path, ".mp4") || ends_ci(path, ".avi") ||
         ends_ci(path, ".mkv") || ends_ci(path, ".mov") ||
         ends_ci(path, ".webm") || ends_ci(path, ".m4v") ||
         ends_ci(path, ".wmv") || ends_ci(path, ".flv") ||
         ends_ci(path, ".f4v") || ends_ci(path, ".3gp") ||
         ends_ci(path, ".3g2") || ends_ci(path, ".ogv") ||
         ends_ci(path, ".mpg") || ends_ci(path, ".mpeg") ||
         ends_ci(path, ".mpe") || ends_ci(path, ".ts") ||
         ends_ci(path, ".mts") || ends_ci(path, ".m2ts") ||
         ends_ci(path, ".vob");
}
void usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s <source-path> <max-px> <out.png>\n", argv0);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    usage(argv[0]);
    return 1;
  }
  const std::string path = argv[1];
  const int max_px = std::atoi(argv[2]);
  const std::string out = argv[3];
  if (max_px <= 0 || max_px > 4096 || out.empty()) {
    usage(argv[0]);
    return 1;
  }

  cairo_surface_t* s = nullptr;
  if (is_video_path(path)) {
    // Synchronous single-shot: ffmpegthumbnailer straight to the output.
    // (In-process path uses VideoThumbWorker queue + cache; the helper has
    // no AppState so it renders directly.)
#ifdef FFMPEGTHUMBNAILER_BIN
    const char* thumb_bin = FFMPEGTHUMBNAILER_BIN;
#else
    const char* thumb_bin = "ffmpegthumbnailer";
#endif
    char cmd[4096];
    std::snprintf(cmd, sizeof(cmd), "\"%s\" -s %d -q 1 -i \"%s\" -o \"%s\" "
                                   "2>/dev/null",
                  thumb_bin, max_px, path.c_str(), out.c_str());
    int rc = std::system(cmd);
    return rc == 0 ? 0 : 2;
  } else if (eh::file_browser::is_svg_extension(path))
    s = eh::file_browser::load_svg_thumbnail(path, max_px);
  else if (eh::file_browser::is_pdf_extension(path))
    s = eh::file_browser::load_pdf_thumbnail(path, max_px);
  else if (eh::file_browser::is_epub_extension(path))
    s = eh::file_browser::load_epub_thumbnail(path, max_px);
  else if (eh::file_browser::is_image_extension(path))
    s = eh::file_browser::load_image_thumbnail(path, max_px);
  else
    return 1;  // unsupported type: caller draws the generic icon

  if (!s) return 2;
  cairo_status_t st = cairo_surface_write_to_png(s, out.c_str());
  cairo_surface_destroy(s);
  return st == CAIRO_STATUS_SUCCESS ? 0 : 2;
}
