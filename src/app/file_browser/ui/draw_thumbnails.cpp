// draw_thumbnails.cpp — thumbnail cache lookup, sync decode and lazy enqueue.
// Moved wholesale from ui/draw.cpp (byte-identical bodies).

#include "../app.hpp"
#include "../features/sidebar/sidebar.hpp"
#include "app/file_browser/features/thumbnails/thumb_pool.hpp"
#include "draw_thumbnails.hpp"

#include <cairo/cairo.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/common/icon_cache/icon_cache.hpp"
#include "app/file_browser/features/preview/svg_preview.hpp"
#include "app/file_browser/features/preview/video_preview.hpp"
#include "app/file_browser/features/preview/pdf_preview.hpp"
#include "app/file_browser/features/preview/epub_preview.hpp"
#include "app/file_browser/features/preview/image_preview.hpp"
#include "app/file_browser/features/thumbnails/thumbnail_cache.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {

void preview_log(const char* fmt, ...);

// ── thumbnail cache ──────────────────────────────────────────────



cairo_surface_t* get_thumbnail(AppState& app, const std::string& path,
                                        int size) {
  auto it = app.thumb_cache.find(path);
  if (it != app.thumb_cache.end())
    return it->second;  // hot-path: no O(n) LRU reorder

  // Check disk cache before decoding
  if (cairo_surface_t* s = load_cached_thumbnail(path, size)) {
    preview_log("get_thumbnail: DISK CACHE HIT path=%s size=%d", path.c_str(), size);
    int sh = cairo_image_surface_get_height(s);
    int stride = cairo_image_surface_get_stride(s);
    app.thumb_cache_bytes += static_cast<std::size_t>(sh * stride);
    app.thumb_cache[path] = s;
    app.thumb_lru.push_front(path);
    return s;
  }

  bool used_video = false;
  cairo_surface_t* s =
      eh::file_browser::thumb_decode_sync(path, size, &used_video);
  if (!s) {
    preview_log("get_thumbnail: FAIL path=%s size=%d video=%d", path.c_str(),
                size, (int)used_video);
    return nullptr;
  }

  while (app.thumb_cache_bytes >= AppState::kThumbCacheMaxBytes &&
         !app.thumb_lru.empty()) {
    auto evict = app.thumb_lru.back();
    auto ev = app.thumb_cache.find(evict);
    if (ev != app.thumb_cache.end()) {
      int eh = cairo_image_surface_get_height(ev->second);
      int estr = cairo_image_surface_get_stride(ev->second);
      app.thumb_cache_bytes -= static_cast<std::size_t>(eh * estr);
      cairo_surface_destroy(ev->second);
      app.thumb_cache.erase(ev);
    }
    app.thumb_lru.pop_back();
  }

  int sh = cairo_image_surface_get_height(s);
  int stride = cairo_image_surface_get_stride(s);
  app.thumb_cache_bytes += static_cast<std::size_t>(sh * stride);
  app.thumb_cache[path] = s;
  app.thumb_lru.push_front(path);
  // Save to disk cache for next time (skip video — already handled by ffmpegthumbnailer)
  if (!used_video)
    save_thumbnail_cache(path, size, s);
  return s;
}

// Pure thumbnail decode: disk cache first, then type-specific loader;
// saves back to disk cache (except video). No shared state touched — safe
// on any thread.
cairo_surface_t* thumb_decode_sync(const std::string& path, int size,
                                   bool* used_video) {
  *used_video = is_video_extension(path);
  if (cairo_surface_t* cached = load_cached_thumbnail(path, size)) return cached;

  cairo_surface_t* s = nullptr;
  if      (*used_video)            s = load_video_thumbnail(path, size);
  else if (is_svg_extension(path)) s = load_svg_thumbnail(path, size);
  else if (is_pdf_extension(path)) s = load_pdf_thumbnail(path, size);
  else if (is_epub_extension(path)) s = load_epub_thumbnail(path, size);
  else if (is_image_extension(path)) s = load_image_thumbnail(path, size);

  if (s && !*used_video) save_thumbnail_cache(path, size, s);
  return s;
}

// Paint-path rule (Dolphin/Nemo): NEVER decode here. Cache hit returns the
// surface; a miss queues background decoding and draws the generic icon
// this frame. The frame loop installs finished surfaces between paints.
cairo_surface_t* get_thumbnail_lazy(AppState& app, int vi,
                                            const std::string& path,
                                            int size) {
  auto it = app.thumb_cache.find(path);
  if (it != app.thumb_cache.end())
    return it->second;  // No LRU reorder here: std::list::remove is O(n) and
                        // this runs for every visible row on every frame.
                        // Recency is tracked at install time instead.
  thumb_pool_enqueue(app, path, size);
  return nullptr;
}

} // namespace eh::file_browser
