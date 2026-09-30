// draw_thumbnails.cpp — thumbnail cache lookup, sync decode and lazy enqueue.
// Moved wholesale from ui/draw.cpp (byte-identical bodies).

#include "../app.hpp"
#include "../features/sidebar/sidebar.hpp"
#include "app/file_browser/features/thumbnails/thumb_pool.hpp"
#include "draw_thumbnails.hpp"

#include <cairo/cairo.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include "platform/common/icon_cache/icon_cache.hpp"
#include "app/file_browser/features/preview/video_preview.hpp"
// Predicate-only headers (bodies live in preview_types.cpp): including them
// pulls no decode-library dependency into horizon-files.
#include "app/file_browser/features/preview/svg_preview.hpp"
#include "app/file_browser/features/preview/pdf_preview.hpp"
#include "app/file_browser/features/preview/epub_preview.hpp"
#include "app/file_browser/features/preview/image_preview.hpp"
#include "app/file_browser/features/thumbnails/thumbnail_cache.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {

void preview_log(const char* fmt, ...);




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

// Out-of-process decode via horizon-thumbnailer(1). Returns the decoded
// surface, or nullptr when the helper is disabled/missing/failed. Image,
// SVG, PDF and EPUB decode ONLY here — their libraries are not linked into
// horizon-files at all. Runs on the thumb-pool worker, never the UI thread.
cairo_surface_t* thumb_via_helper(const std::string& path, int size) {
  if (const char* e = std::getenv("EH_THUMB_HELPER"))
    if (*e && e[0] == '0') return nullptr;
  // Unsupported types cost a spawn; pre-filter to the helper's set.
  if (!is_video_extension(path) && !is_svg_extension(path) &&
      !is_pdf_extension(path) && !is_epub_extension(path) &&
      !is_image_extension(path))
    return nullptr;

  char tmpl[] = "/tmp/horizon-thumb-XXXXXX.png";
  int fd = ::mkstemps(tmpl, 4);
  if (fd < 0) return nullptr;
  ::close(fd);

  pid_t pid = ::fork();
  if (pid < 0) {
    ::unlink(tmpl);
    return nullptr;
  }
  if (pid == 0) {
    int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    char px[16];
    std::snprintf(px, sizeof(px), "%d", size);
    // Installed location (or PATH) first; EH_THUMB_HELPER_BIN overrides.
    if (const char* bin = std::getenv("EH_THUMB_HELPER_BIN"))
      ::execl(bin, "horizon-thumbnailer", path.c_str(), px, tmpl,
              (char*)nullptr);
    ::execlp("horizon-thumbnailer", "horizon-thumbnailer", path.c_str(), px,
             tmpl, (char*)nullptr);
    _exit(127);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  cairo_surface_t* s = nullptr;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    s = cairo_image_surface_create_from_png(tmpl);
  ::unlink(tmpl);
  if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    if (s) cairo_surface_destroy(s);
    return nullptr;
  }
  return s;
}

// Thumbnail decode: disk cache first, then the out-of-process helper.
// No in-process image/svg/pdf/epub decoders remain in horizon-files —
// poppler, librsvg, libjpeg and libwebp link only into horizon-thumbnailer.
// No shared state touched — safe on any thread.
cairo_surface_t* thumb_decode_sync(const std::string& path, int size,
                                   bool* used_video) {
  *used_video = is_video_extension(path);
  if (cairo_surface_t* cached = load_cached_thumbnail(path, size)) return cached;
  if (cairo_surface_t* h = thumb_via_helper(path, size)) {
    // Save helper results back to disk cache (skip video — ffmpegthumbnailer
    // manages its own cache) so repeat views never respawn.
    if (!*used_video) save_thumbnail_cache(path, size, h);
    return h;
  }
  if (*used_video) return load_video_thumbnail(path, size);
  return nullptr;
}

// Paint-path rule: NEVER decode here. Cache hit returns the
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
