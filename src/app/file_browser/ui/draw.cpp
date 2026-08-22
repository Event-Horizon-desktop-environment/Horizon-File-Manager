#include "../app.hpp"
#include "../trace.hpp"
#include "../features/view_zoom.hpp"
#include "app/file_browser/features/thumb_pool.hpp"
#include "app/file_browser/features/dir_stats.hpp"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

// ── icon debug logger ──────────────────────────────────────────────
static std::mutex g_icon_log_mtx;
static void icon_log(const char* fmt, ...) {
  static FILE* f = nullptr;
  std::lock_guard<std::mutex> lock(g_icon_log_mtx);
  if (!f) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/horizon-files-icons.log";
    f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "icon log started\n");
    fflush(f);
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fprintf(f, "\n");
  fflush(f);
}

#include "platform/common/icon_cache/icon_cache.hpp"
#include "app/file_browser/features/svg_preview.hpp"
#include "app/file_browser/features/video_preview.hpp"
#include "app/file_browser/features/pdf_preview.hpp"
#include "app/file_browser/features/epub_preview.hpp"
#include "app/file_browser/features/image_preview.hpp"
#include "app/file_browser/features/thumbnail_cache.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {

// ── preview/thumbnail debug logger ─────────────────────────────────
static std::mutex g_preview_log_mtx;
void preview_log(const char* fmt, ...) {
  static FILE* f = nullptr;
  std::lock_guard<std::mutex> lock(g_preview_log_mtx);
  if (!f) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/horizon-files-previews.log";
    f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "preview log started\n");
    fflush(f);
  }
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm* t = localtime(&ts.tv_sec);
  fprintf(f, "%02d:%02d:%02d.%03ld ", t->tm_hour, t->tm_min, t->tm_sec, ts.tv_nsec / 1000000);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fprintf(f, "\n");
  fflush(f);
}

static bool trash_has_files() {
  const char* home = std::getenv("HOME");
  if (!home) return false;
  fs::path trash_files = fs::path(home) / ".local/share/Trash/files";
  std::error_code ec;
  fs::directory_iterator it(trash_files, ec);
  if (ec) return false;
  for (auto& entry : it) {
    (void)entry;
    return true;
  }
  return false;
}

// ── drawing helpers ──────────────────────────────────────────────

void draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h,
                       double r) {
  cairo_new_path(cr);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_arc(cr, x + w - r, y + r, r, 3 * M_PI / 2, 2 * M_PI);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_close_path(cr);
}

void draw_scrollbar(AppState& app, cairo_t* cr, int x, int y, int h,
                    int content_h, int view_h, int scroll_px, double r,
                    double g, double b, bool computer_view) {
  if (content_h <= view_h) return;

  // Record the interactive region so input can hit-test/drag this thumb.
  app.scrollbar_rects.push_back({x, y, 6, h, content_h, view_h, computer_view});

  double thumb_h = static_cast<double>(view_h) * static_cast<double>(view_h) /
                   static_cast<double>(content_h);
  double max_scroll = static_cast<double>(content_h - view_h);
  double thumb_y =
      static_cast<double>(scroll_px) / max_scroll * (view_h - thumb_h);

  cairo_set_source_rgba(cr, r, g, b, 0.15);
  cairo_rectangle(cr, static_cast<double>(x), static_cast<double>(y), 6.0,
                  static_cast<double>(h));
  cairo_fill(cr);

  cairo_set_source_rgba(cr, r, g, b, 0.4);
  draw_rounded_rect(cr, static_cast<double>(x), y + thumb_y, 6.0,
                    std::max(thumb_h, 20.0), 3.0);
  cairo_fill(cr);
}

// ── thumbnail cache ──────────────────────────────────────────────

cairo_surface_t* eh_thumb_decode(const std::string& path, int size,
                                 bool* used_video);

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
static cairo_surface_t* get_thumbnail_lazy(AppState& app, int vi,
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

static bool is_pdf_ext(const std::string& path) {
  if (path.size() < 5) return false;
  const char* s = path.c_str();
  size_t i = path.size() - 4;
  return (s[i] == '.' || s[i] == '.') &&
         (s[i+1] == 'p' || s[i+1] == 'P') &&
         (s[i+2] == 'd' || s[i+2] == 'D') &&
         (s[i+3] == 'f' || s[i+3] == 'F');
}

static const char* icon_name_for_file_type(FileType ft, const std::string* file_path = nullptr) {
  if (ft == FileType::Document && file_path && is_pdf_ext(*file_path)) {
    icon_log("[icon] PDF detected: path=%s -> icon=application-pdf", file_path->c_str());
    return "application-pdf";
  }
  const char* name;
  switch (ft) {
    case FileType::Folder:     name = "folder"; break;
    case FileType::Image:      name = "image-x-generic"; break;
    case FileType::Audio:      name = "audio-x-generic"; break;
    case FileType::Video:      name = "video-x-generic"; break;
    case FileType::Text:       name = "text-x-generic"; break;
    case FileType::Markdown:   name = "text-x-markdown"; break;
    case FileType::Code:       name = "text-x-code"; break;
    case FileType::Document:   name = "x-office-document"; break;
    case FileType::Font:       name = "font-x-generic"; break;
    case FileType::Archive:    name = "application-x-archive"; break;
    case FileType::Executable: name = "application-x-executable"; break;
    case FileType::Web:        name = "text-html"; break;
    default:                   name = "text-x-generic"; break;
  }
  icon_log("[icon] ft=%d path=%s -> icon=%s", static_cast<int>(ft),
           file_path ? file_path->c_str() : "(null)", name);
  return name;
}

// Small status badge drawn over a file icon's lower-left corner.
// kind: 0 = symlink (accent arrow), 1 = read-only (padlock), 2 = no access,
//       3 = locked by root (amber padlock with keyhole)
static void draw_emblem_badge(AppState& app, cairo_t* cr, double cx, double cy,
                              double r, int kind) {
  // Backing disc
  if (kind == 0) {
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
  } else if (kind == 1) {
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
  } else if (kind == 3) {
    cairo_set_source_rgba(cr, 0.95, 0.62, 0.10, 1.0);
  } else {
    cairo_set_source_rgba(cr, 0.85, 0.25, 0.25, 1.0);
  }
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  cairo_fill(cr);
  cairo_set_line_width(cr, std::max(1.0, r * 0.14));
  cairo_set_source_rgba(cr, 1, 1, 1, 0.9);

  if (kind == 0) {
    // Diagonal arrow (symlink)
    double a = r * 0.45;
    cairo_move_to(cr, cx - a, cy + a);
    cairo_line_to(cr, cx + a * 0.8, cy - a * 0.8);
    cairo_stroke(cr);
    // Arrowhead
    cairo_move_to(cr, cx + a * 0.8, cy - a * 0.8);
    cairo_line_to(cr, cx + a * 0.8 - a * 0.55, cy - a * 0.8);
    cairo_stroke(cr);
    cairo_move_to(cr, cx + a * 0.8, cy - a * 0.8);
    cairo_line_to(cr, cx + a * 0.8, cy - a * 0.8 + a * 0.55);
    cairo_stroke(cr);
  } else if (kind == 1 || kind == 3) {
    // Padlock: shackle arc + body (+ keyhole for root lock)
    double bw = r * 0.9, bh = r * 0.7;
    cairo_rectangle(cr, cx - bw / 2, cy - bh * 0.1, bw, bh);
    cairo_fill(cr);
    cairo_set_line_width(cr, std::max(1.0, r * 0.18));
    cairo_arc(cr, cx, cy - bh * 0.1, bw * 0.32, M_PI, 2 * M_PI);
    cairo_stroke(cr);
    if (kind == 3) {
      cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
      cairo_arc(cr, cx, cy + bh * 0.25, std::max(0.8, r * 0.13), 0, 2 * M_PI);
      cairo_fill(cr);
    }
  } else {
    // "No entry" bar
    double bw = r * 1.1, bh = r * 0.28;
    cairo_rectangle(cr, cx - bw / 2, cy - bh / 2, bw, bh);
    cairo_fill(cr);
  }
}

static void draw_file_icon_body(AppState& app, cairo_t* cr, int x, int y,
                                  int size, FileType ft, bool selected,
                                  const std::string& icon_name = {},
                                  cairo_surface_t* thumb = nullptr,
                                  const std::string* file_path = nullptr) {
  // Types that support thumbnails: draw thumb first if available
  bool has_thumb = thumb != nullptr;
  bool thumb_type = (ft == FileType::Image || ft == FileType::Video);
  bool doc_thumb = (ft == FileType::Document && thumb && file_path
                    && (is_pdf_extension(*file_path) || is_epub_extension(*file_path)));

  if ((thumb_type || doc_thumb) && has_thumb) {
    double iw = static_cast<double>(cairo_image_surface_get_width(thumb));
    double ih = static_cast<double>(cairo_image_surface_get_height(thumb));
    if (iw > 0 && ih > 0) {
      double scale = size / std::max(1.0, std::max(iw, ih));
      cairo_save(cr);
      // White page background behind PDF thumbnails
      if (file_path && is_pdf_extension(*file_path)) {
        double bw = iw * scale;
        double bh = ih * scale;
        double bx = x + (size - bw) / 2;
        double by = y + (size - bh) / 2;
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_fill(cr);
      }
      double rad = 6;
      cairo_new_path(cr);
      cairo_arc(cr, x + rad, y + rad, rad, M_PI, 3 * M_PI / 2);
      cairo_arc(cr, x + size - rad, y + rad, rad, 3 * M_PI / 2, 2 * M_PI);
      cairo_arc(cr, x + size - rad, y + size - rad, rad, 0, M_PI / 2);
      cairo_arc(cr, x + rad, y + size - rad, rad, M_PI / 2, M_PI);
      cairo_close_path(cr);
      cairo_clip(cr);
      cairo_translate(cr, x, y);
      cairo_scale(cr, scale, scale);
      cairo_set_source_surface(
          cr, thumb, (size / scale - iw) / 2, (size / scale - ih) / 2);
      cairo_paint(cr);
      cairo_restore(cr);
      return;
    }
  }

  // Fallback to per-entry icon name (MIME-type-derived) if set
  if (!icon_name.empty()) {
    const auto* ic = app.icons.tray_icon(icon_name, size);
    if (ic && ic->surface) {
      double iw = static_cast<double>(ic->width);
      double ih = static_cast<double>(ic->height);
      if (iw > 0 && ih > 0) {
        double scale = size / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, ic->surface,
                                 (size / scale - iw) / 2,
                                 (size / scale - ih) / 2);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
      }
    }
  }

  const char* icon_name_resolved = icon_name_for_file_type(ft, file_path);
  const auto* ic = app.icons.tray_icon(icon_name_resolved, size);
  if (ic && ic->surface) {
    double iw = static_cast<double>(ic->width);
    double ih = static_cast<double>(ic->height);
    if (iw > 0 && ih > 0) {
      icon_log("[icon] FOUND in theme: name=%s %dx%d", icon_name_resolved, ic->width, ic->height);
      double scale = size / std::max(1.0, std::max(iw, ih));
      cairo_save(cr);
      cairo_translate(cr, x, y);
      cairo_scale(cr, scale, scale);
      cairo_set_source_surface(cr, ic->surface,
                               (size / scale - iw) / 2,
                               (size / scale - ih) / 2);
      cairo_paint(cr);
      cairo_restore(cr);
      return;
    }
  }
  icon_log("[icon] NOT FOUND in theme: name=%s -> using fallback color", icon_name_resolved);

  double r, g, b;
  switch (ft) {
    case FileType::Folder:     r = 0.85; g = 0.65; b = 0.20; break;
    case FileType::Image:      r = 0.20; g = 0.75; b = 0.40; break;
    case FileType::Audio:      r = 0.30; g = 0.55; b = 0.90; break;
    case FileType::Video:      r = 0.70; g = 0.35; b = 0.80; break;
    case FileType::Text:       r = 0.50; g = 0.50; b = 0.55; break;
    case FileType::Markdown:   r = 0.25; g = 0.50; b = 0.70; break;
    case FileType::Code:       r = 0.30; g = 0.60; b = 0.55; break;
    case FileType::Document:   r = 0.20; g = 0.45; b = 0.75; break;
    case FileType::Font:       r = 0.55; g = 0.35; b = 0.70; break;
    case FileType::Archive:    r = 0.70; g = 0.50; b = 0.20; break;
    case FileType::Executable: r = 0.60; g = 0.40; b = 0.25; break;
    case FileType::Web:        r = 0.30; g = 0.55; b = 0.85; break;
    default:                   r = 0.40; g = 0.40; b = 0.45; break;
  }
  if (selected) { r = r * 1.3; g = g * 1.3; b = b * 1.3; }

  cairo_save(cr);
  double rad = size * 0.2;
  cairo_new_path(cr);
  cairo_arc(cr, x + rad, y + rad, rad, M_PI, 3 * M_PI / 2);
  cairo_arc(cr, x + size - rad, y + rad, rad, 3 * M_PI / 2, 2 * M_PI);
  cairo_arc(cr, x + size - rad, y + size - rad, rad, 0, M_PI / 2);
  cairo_arc(cr, x + rad, y + size - rad, rad, M_PI / 2, M_PI);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, r, g, b, 1.0);
  cairo_fill(cr);

  const char* label = "?";
  switch (ft) {
    case FileType::Folder:     label = "F"; break;
    case FileType::Image:      label = "I"; break;
    case FileType::Audio:      label = "A"; break;
    case FileType::Video:      label = "V"; break;
    case FileType::Text:       label = "T"; break;
    case FileType::Markdown:   label = "M"; break;
    case FileType::Code:       label = "C"; break;
    case FileType::Document:   label = "D"; break;
    case FileType::Font:       label = "f"; break;
    case FileType::Archive:    label = "Z"; break;
    case FileType::Executable: label = "X"; break;
    case FileType::Web:        label = "W"; break;
    default:                   label = "?"; break;
  }
  cairo_set_source_rgba(cr, 1, 1, 1, 1.0);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, size * 0.45);
  cairo_text_extents_t te;
  cairo_text_extents(cr, label, &te);
  cairo_move_to(cr, x + (size - te.width) / 2 - te.x_bearing,
                y + (size + te.height) / 2 - te.y_bearing);
  cairo_show_text(cr, label);
  cairo_restore(cr);
}

// Icon + status emblems (symlink / read-only / no access / root lock)
static void draw_file_icon_cairo(AppState& app, cairo_t* cr, int x, int y,
                                  int size, FileType ft, bool selected,
                                  const std::string& icon_name = {},
                                  cairo_surface_t* thumb = nullptr,
                                  const std::string* file_path = nullptr,
                                  const FileEntry* entry = nullptr) {
  draw_file_icon_body(app, cr, x, y, size, ft, selected, icon_name, thumb, file_path);
  if (!entry) return;

  // Skip emblems on tiny icons
  if (size < 24) return;

  double r = std::max(5.0, size * 0.11);
  double pad = r * 0.4;
  double cy = y + size - r - pad;
  double cx = x + r + pad;
  int drawn = 0;

  // Root-owned entries the current user cannot modify
  bool locked_by_root = entry->owned_by_root && geteuid() != 0;

  if (entry->is_symlink) {
    draw_emblem_badge(app, cr, cx + drawn * (r * 2 + pad * 1.6), cy, r, 0);
    ++drawn;
  }
  if (locked_by_root) {
    draw_emblem_badge(app, cr, cx + drawn * (r * 2 + pad * 1.6), cy, r, 3);
    ++drawn;
  } else if (entry->readable && !entry->writable) {
    draw_emblem_badge(app, cr, cx + drawn * (r * 2 + pad * 1.6), cy, r, 1);
    ++drawn;
  }
  if (!entry->readable || (entry->is_dir && entry->mode &&
                           !(entry->mode & S_IXUSR))) {
    draw_emblem_badge(app, cr, cx + drawn * (r * 2 + pad * 1.6), cy, r, 2);
  }
}

static std::string format_size(uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
  if (bytes < 1024ULL * 1024 * 1024)
    return std::to_string(bytes / (1024 * 1024)) + " MB";
  return std::to_string(bytes / (1024ULL * 1024 * 1024)) + " GB";
}

// ── sidebar ──────────────────────────────────────────────────────

void draw_sidebar(AppState& app, cairo_t* cr, int sidebar_w, int top_y,
                  int) {
  // One-shot: log first-paint sub-phases to find cold-start hot spots.
  struct SubMark { const char* n; };
  auto sub_t0 = std::chrono::steady_clock::now();
  auto sub = [&, tag = ""](const char* n) mutable {
    static int calls = 0;
    if (calls++ > 6) return;   // only the first paint's marks
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - sub_t0).count();
    if (trace::enabled().load(std::memory_order_relaxed))
      trace::log("SIDEBAR FIRST %s %.2f ms", n, ms);
    sub_t0 = std::chrono::steady_clock::now();
    (void)tag;
  };
  double zf = app.zoom_pct / 100.0;
  int total = static_cast<int>(app.sidebar_locations.size());
  // Find section boundaries in sidebar_locations order:
  //   Places (Home..Trash) → Favorites → Drives (Root, Drive)
  int places_end = 0;
  while (places_end < total &&
         app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Favorite &&
         app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Root &&
         app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Drive)
    ++places_end;

  int fav_start = places_end;
  while (fav_start < total &&
         app.sidebar_locations[fav_start].kind == SidebarLocation::Kind::Favorite)
    ++fav_start;

  int drives_start = fav_start;

  // Compute total sidebar content height for scroll clamping
  // Items keep a fixed readable size and overflow scrolls
  {
    int p_count = places_end;
    int f_count = fav_start - places_end;
    int d_count = total - drives_start;
    int item_h = static_cast<int>(36 * 1.2);
    int drive_extra_h = static_cast<int>(16 * 1.2);
    int header_h = static_cast<int>(24 * 1.2);
    int div_total = static_cast<int>(17 * 1.2);
    int drives_h = 0;
    for (int i = drives_start; i < total; ++i) {
      const auto& loc = app.sidebar_locations[i];
      drives_h += item_h;
      if ((loc.kind == SidebarLocation::Kind::Drive ||
           loc.kind == SidebarLocation::Kind::Root) &&
          loc.total_bytes > 0 && loc.is_mounted)
        drives_h += drive_extra_h;
    }
    int total_needed = header_h + p_count * item_h +
                       div_total + header_h + f_count * item_h +
                       div_total + header_h + drives_h;
    int available = app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height;
    app.sidebar_content_h = total_needed;
    // Clamp scroll to prevent blank space below last item
    int max_scroll = std::max(0, total_needed - available);
    if (app.sidebar_scroll_px > max_scroll)
      app.sidebar_scroll_px = max_scroll;
  }
  zf = 1.2;

  // HTML has py-6 (24px) padding at top of sidebar
  int y = top_y - app.sidebar_scroll_px + static_cast<int>(24.0 * zf);

  auto draw_item = [&](int idx) {
    const auto& loc = app.sidebar_locations[idx];
    bool has_usage = (loc.kind == SidebarLocation::Kind::Drive ||
                      loc.kind == SidebarLocation::Kind::Root) &&
                     loc.total_bytes > 0 && loc.is_mounted;
    int item_h = has_usage ? static_cast<int>(52.0 * zf) : static_cast<int>(36.0 * zf);
    bool hovered = (idx == app.sidebar_hover_idx);
    bool drop_target = app.drop_target_is_sidebar && idx == app.drop_target_sidebar_idx;

    int sb_margin = static_cast<int>(8.0 * zf); // px-4 in HTML

    if (drop_target) {
      double pulse = 0.16 + 0.06 * std::sin(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count() * 0.006);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, pulse);
      draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h,
                        static_cast<int>(12.0 * zf)); // rounded-2xl
      cairo_fill(cr);
      app.pendingRedraw = true;
    } else if (hovered) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h,
                        static_cast<int>(12.0 * zf)); // rounded-2xl
      cairo_fill(cr);
    }

    // Icon (w-5 = 20px)
    int icon_sz = static_cast<int>(20.0 * zf);
    int icon_x = static_cast<int>(16.0 * zf); // px-4

    cairo_surface_t* cs = nullptr;
    switch (loc.kind) {
      case SidebarLocation::Kind::Desktop:   cs = app.icon_desktop_svg; break;
      case SidebarLocation::Kind::Documents: cs = app.icon_documents_svg; break;
      case SidebarLocation::Kind::Downloads: cs = app.icon_downloads_svg; break;
      case SidebarLocation::Kind::Music:     cs = app.icon_music_svg; break;
      case SidebarLocation::Kind::Pictures:  cs = app.icon_pictures_svg; break;
      case SidebarLocation::Kind::Videos:    cs = app.icon_videos_svg; break;
      default: break;
    }

    auto draw_icon_at = [&](int ix, int iy) {
      if (cs) {
        double iw = static_cast<double>(cairo_image_surface_get_width(cs));
        double ih = static_cast<double>(cairo_image_surface_get_height(cs));
        double scale = icon_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_translate(cr, ix, iy);
        cairo_scale(cr, scale, scale);
        cairo_rectangle(cr, 0, 0, icon_sz / scale, icon_sz / scale);
        cairo_clip(cr);
        cairo_mask_surface(cr, cs, (icon_sz / scale - iw) / 2, (icon_sz / scale - ih) / 2);
        cairo_restore(cr);
      } else {
        const char* icon_name = loc.icon_name.c_str();
        std::string trash_icon;
        if (loc.kind == SidebarLocation::Kind::Trash) {
          trash_icon = trash_has_files() ? "user-trash-full" : "user-trash";
          icon_name = trash_icon.c_str();
        }
        // Async resolve: never decode theme SVGs on the paint thread.
        // The placeholder letter shows for a frame or two, then the real
        // icon pops in (catch-up frames are scheduled right after startup).
        const auto* ic = [&]{
          auto ti0 = std::chrono::steady_clock::now();
          const auto* r = app.icons.tray_icon(icon_name, icon_sz);
          static std::atomic<int> tic{0};
          if (tic.fetch_add(1, std::memory_order_relaxed) < 12 &&
              trace::enabled().load(std::memory_order_relaxed))
            trace::log("SIDEBAR tray[%s] %.2f ms", icon_name,
                       std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - ti0).count());
          return r;
        }();
        if (ic && ic->surface) {
          double iw = static_cast<double>(ic->width);
          double ih = static_cast<double>(ic->height);
          double scale = icon_sz / std::max(1.0, std::max(iw, ih));
          cairo_save(cr);
          cairo_translate(cr, ix, iy);
          cairo_scale(cr, scale, scale);
          cairo_set_source_surface(cr, ic->surface, (icon_sz / scale - iw) / 2,
                                    (icon_sz / scale - ih) / 2);
          cairo_paint(cr);
          cairo_restore(cr);
        } else {
          cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
          cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                  CAIRO_FONT_WEIGHT_NORMAL);
          cairo_set_font_size(cr, 16.0 * zf);
          cairo_move_to(cr, ix, iy + icon_sz);
          char fallback[2] = { loc.label.empty() ? '?' : loc.label[0], '\0' };
          cairo_show_text(cr, fallback);
        }
      }
    };

    // For items with extra usage row, center icon in the main row (top 36px)
    int main_row_h = static_cast<int>(36.0 * zf);
    {
      auto mi0 = std::chrono::steady_clock::now();
      draw_icon_at(icon_x, y + (main_row_h - icon_sz) / 2);
      static std::atomic<int> mic{0};
      if (mic.fetch_add(1, std::memory_order_relaxed) < 12 &&
          trace::enabled().load(std::memory_order_relaxed)) {
        trace::log("SIDEBAR icon[%s] %.2f ms", loc.label.c_str(),
                   std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - mi0)
                       .count());
      }
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0 * zf); // text-sm = 14px, close enough
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);

    // gap-3 = 12px between icon and label
    int label_x = icon_x + icon_sz + static_cast<int>(12.0 * zf);

    if (loc.kind == SidebarLocation::Kind::Drive || loc.kind == SidebarLocation::Kind::Root) {
      int max_label_w = sidebar_w - label_x - static_cast<int>(40.0 * zf);
      cairo_text_extents_t te;
      cairo_text_extents(cr, loc.label.c_str(), &te);
      if (te.x_advance > max_label_w) {
        std::string short_label = loc.label;
        while (!short_label.empty()) {
          cairo_text_extents(cr, (short_label + "...").c_str(), &te);
          if (te.x_advance <= max_label_w) break;
          short_label.pop_back();
        }
        cairo_move_to(cr, label_x, y + main_row_h / 2 + static_cast<int>(4.0 * zf));
        cairo_show_text(cr, (short_label + "...").c_str());
      } else {
        cairo_move_to(cr, label_x, y + main_row_h / 2 + static_cast<int>(4.0 * zf));
        cairo_show_text(cr, loc.label.c_str());
      }
    } else {
      cairo_move_to(cr, label_x, y + item_h / 2 + static_cast<int>(4.0 * zf));
      cairo_show_text(cr, loc.label.c_str());
    }

    // Mount indicator for drives
    if (loc.kind == SidebarLocation::Kind::Drive) {
      if (loc.is_mounted && app.mounted_svg) {
        int ind_sz = static_cast<int>(18.0 * zf);
        int ind_x = sidebar_w - static_cast<int>(24.0 * zf);
        int ind_y = y + (main_row_h - ind_sz) / 2;
        // Hover background
        if (idx == app.sidebar_mount_hover_idx) {
          cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                                 app.text_secondary_b, 0.12);
          double rad = ind_sz * 0.5 + 4;
          cairo_arc(cr, ind_x + ind_sz / 2.0, ind_y + ind_sz / 2.0, rad, 0, 2 * M_PI);
          cairo_fill(cr);
        }
        double iw = static_cast<double>(cairo_image_surface_get_width(app.mounted_svg));
        double ih = static_cast<double>(cairo_image_surface_get_height(app.mounted_svg));
        double scale = ind_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                               app.text_secondary_b,
                               idx == app.sidebar_mount_hover_idx ? 1.0 : 0.7);
        cairo_rectangle(cr, ind_x, ind_y, ind_sz, ind_sz);
        cairo_clip(cr);
        cairo_translate(cr, ind_x, ind_y);
        cairo_scale(cr, scale, scale);
        cairo_translate(cr, (ind_sz / scale - iw) / 2, (ind_sz / scale - ih) / 2);
        cairo_mask_surface(cr, app.mounted_svg, 0, 0);
        cairo_restore(cr);
      }
    }

    // Usage progress bar for drives with data
    if (has_usage) {
      int usage_y = y + main_row_h;
      int bar_x = icon_x;
      int bar_w = sidebar_w - bar_x - static_cast<int>(12.0 * zf);
      int bar_h = static_cast<int>(4.0 * zf);
      int bar_y = usage_y + static_cast<int>(10.0 * zf);

      // Usage text
      uint64_t used = loc.total_bytes - loc.free_bytes;
      std::string usage_text = format_size(used) + " / " + format_size(loc.total_bytes);
      cairo_set_font_size(cr, 10.0 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                             app.text_secondary_b, 0.8);
      cairo_move_to(cr, bar_x, bar_y - static_cast<int>(4.0 * zf));
      cairo_show_text(cr, usage_text.c_str());

      double frac = std::min(1.0, static_cast<double>(used) /
                                   static_cast<double>(loc.total_bytes));

      // Track
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
      draw_rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, static_cast<int>(2.0 * zf));
      cairo_fill(cr);

      // Fill
      if (frac > 0.01) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
        draw_rounded_rect(cr, bar_x, bar_y, static_cast<double>(bar_w) * frac,
                          bar_h, static_cast<int>(2.0 * zf));
        cairo_fill(cr);
      }
    }

    y += item_h;
  };

  auto draw_header = [&](const char* text) {
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                           app.text_secondary_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0 * zf); // text-xs
    cairo_move_to(cr, static_cast<int>(16.0 * zf), y + static_cast<int>(14.0 * zf));
    cairo_show_text(cr, text);
    y += static_cast<int>(24.0 * zf);
  };

  auto draw_divider = [&]() {
    y += static_cast<int>(16.0 * zf);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.15);
    int div_margin = static_cast<int>(16.0 * zf);
    cairo_rectangle(cr, div_margin, y, sidebar_w - div_margin * 2, 1);
    cairo_fill(cr);
    y += 1 + static_cast<int>(16.0 * zf);
  };

  sub("pre");
  // ── PLACES ──
  draw_header("PLACES");
  for (int i = 0; i < std::min(places_end, total); ++i)
    draw_item(i);

  sub("places");
  // ── FAVORITES ──
  if (fav_start > places_end) {
    draw_divider();
    draw_header("FAVORITES");

    // Draw "drop to add" indicator when dragging over the section
    bool fav_hover = app.drop_target_fav_section;

    int item_h = static_cast<int>(36.0 * zf);
    bool dragging = app.sidebar_fav_dragging;
    int drag_sb_idx = dragging ? places_end + app.sidebar_fav_drag_from : -1;

    for (int i = places_end; i < std::min(fav_start, total); ++i) {
      // Draw insertion line before this item if slot matches
      if (dragging) {
        int slot = i - places_end;
        if (slot == app.sidebar_fav_drag_to_visual && slot != app.sidebar_fav_drag_from) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.6);
          cairo_set_line_width(cr, 2.0);
          int div_margin = static_cast<int>(16.0 * zf);
          cairo_move_to(cr, div_margin, y);
          cairo_line_to(cr, sidebar_w - div_margin, y);
          cairo_stroke(cr);
        }
      }
      if (dragging && i == drag_sb_idx) {
        // Draw the dragged item dimmed
        cairo_push_group(cr);
        draw_item(i);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.35);
      } else {
        draw_item(i);
      }
    }
    // Insertion line at the end of the list
    if (dragging) {
      int last_slot = fav_start - places_end;
      if (last_slot == app.sidebar_fav_drag_to_visual && last_slot != app.sidebar_fav_drag_from) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.6);
        cairo_set_line_width(cr, 2.0);
        int div_margin = static_cast<int>(16.0 * zf);
        cairo_move_to(cr, div_margin, y);
        cairo_line_to(cr, sidebar_w - div_margin, y);
        cairo_stroke(cr);
      }
    }

    // Ghost — translucent preview of the dragged item following the cursor
    if (dragging && drag_sb_idx >= places_end && drag_sb_idx < fav_start) {
      int ghost_y = app.sidebar_fav_drag_current_y - item_h / 2;
      int saved_y = y;
      y = ghost_y;
      // Accent background
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
      int sb_margin = static_cast<int>(6.0 * zf);
      draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h, sb_margin);
      cairo_fill(cr);
      // Accent border
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.5);
      cairo_set_line_width(cr, 1.5);
      draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h, sb_margin);
      cairo_stroke(cr);
      // Item content
      cairo_push_group(cr);
      draw_item(drag_sb_idx);
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.85);
      y = saved_y;
    }

    // If dragging over Favorites (external drop), draw a "+" indicator at the bottom
    if (fav_hover) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.20);
      int sb_margin = static_cast<int>(6.0 * zf);
      draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h, sb_margin);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 18.0 * zf);
      cairo_move_to(cr, static_cast<int>(16.0 * zf), y + item_h / 2 + static_cast<int>(6.0 * zf));
      cairo_show_text(cr, "+");
      cairo_move_to(cr, static_cast<int>(38.0 * zf), y + item_h / 2 + static_cast<int>(5.0 * zf));
      cairo_set_font_size(cr, 13.0 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.7);
      cairo_show_text(cr, "Add to Favorites");
      y += item_h;
    }
  } else if (app.drop_target_fav_section) {
    // Empty Favorites section — draw placeholder indicator
    draw_divider();
    draw_header("FAVORITES");
    int item_h = static_cast<int>(36.0 * zf);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.20);
    int sb_margin = static_cast<int>(6.0 * zf);
    draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h, sb_margin);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 18.0 * zf);
    cairo_move_to(cr, static_cast<int>(16.0 * zf), y + item_h / 2 + static_cast<int>(6.0 * zf));
    cairo_show_text(cr, "+");
    cairo_move_to(cr, static_cast<int>(38.0 * zf), y + item_h / 2 + static_cast<int>(5.0 * zf));
    cairo_set_font_size(cr, 13.0 * zf);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.7);
    cairo_show_text(cr, "Add to Favorites");
    y += item_h;
  }

  sub("favorites");
  // ── DRIVES ──
  if (drives_start < total) {
    draw_divider();
    draw_header("DRIVES");
    for (int i = drives_start; i < total; ++i)
      draw_item(i);
  }
}

// ── top bar ──────────────────────────────────────────────────────

static void draw_house_icon(cairo_t* cr, int x, int y, int size) {
  cairo_save(cr);
  cairo_translate(cr, static_cast<double>(x), static_cast<double>(y));
  double s = static_cast<double>(size) / 12.0;
  cairo_set_line_width(cr, 2.0 * s);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  // Roof
  cairo_move_to(cr, 0 * s, 6 * s);
  cairo_line_to(cr, 6 * s, 0 * s);
  cairo_line_to(cr, 12 * s, 6 * s);
  cairo_stroke(cr);
  // Walls
  cairo_rectangle(cr, 2 * s, 5 * s, 8 * s, 7 * s);
  cairo_stroke(cr);
  // Door
  cairo_rectangle(cr, 4 * s, 7 * s, 4 * s, 5 * s);
  cairo_stroke(cr);
  cairo_restore(cr);
}

// ── filter label arrays ──────────────────────────────────────────

static constexpr const char* kFilterTypeLabels[] = {
  "All", "Folder", "Image", "Audio", "Video", "Text", "Document",
  "Archive", "Code", "Executable", "Web", "Font", "Markdown",
};

static constexpr const char* kFilterSizeLabels[] = {
  "Any", "< 10 KB", "10-100 KB", "100 KB-1 MB", "1-10 MB", "10-100 MB", "> 100 MB",
};

static constexpr const char* kFilterDateLabels[] = {
  "Any", "Today", "This week", "This month", "This year",
};

static constexpr const char* kFilterTypeShort[] = {
  "All", "Folder", "Img", "Aud", "Vid", "Text", "Doc",
  "Arch", "Code", "Exec", "Web", "Font", "MD",
};

static constexpr const char* kFilterSizeShort[] = {
  "Any", "<10K", "10-100K", "100K-1M", "1-10M", "10-100M", ">100M",
};

static constexpr const char* kFilterDateShort[] = {
  "Any", "Today", "Week", "Month", "Year",
};

void draw_top_bar(AppState& app, cairo_t* cr, int w, int top_h, int pane_x, int pane_w) {
  double zf = app.zoom_pct / 100.0;

  int sidebar_w;
  int content_right;
  if (pane_w > 0) {
    sidebar_w = pane_x;
    content_right = pane_x + pane_w;
  } else {
    sidebar_w = app.sidebar_expanded ? app.sidebar_width : 0;
    content_right = w;
  }

  // Window controls position (traffic lights always on the right)
  if (pane_w == 0) {
    app.win_btn_x = 0;
    app.win_btn_w = static_cast<int>(16.0 * zf);
  }

  // In split view, the global bar only draws window controls (per-pane bars draw everything else)
  if (!app.split_view || pane_w > 0) {

  // ── Navigation arrows (back, forward) ──
  int x = sidebar_w + static_cast<int>(20.0 * zf); // px-5

  auto draw_arrow = [&](int idx, cairo_surface_t* svg, const char* fallback, bool hovered) {
    int slot_w = static_cast<int>(36.0 * zf);
    if (hovered) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, x, (top_h - slot_w) / 2, slot_w, slot_w,
                        static_cast<int>(6.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    if (svg) {
      double svg_w = static_cast<double>(cairo_image_surface_get_width(svg));
      double svg_h = static_cast<double>(cairo_image_surface_get_height(svg));
      int sz = static_cast<int>(18.0 * zf);
      int ox = x + (slot_w - sz) / 2;
      int oy = (top_h - sz) / 2;
      double display_scale = sz / std::max(svg_w, svg_h);
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_rectangle(cr, ox, oy, sz, sz);
      cairo_clip(cr);
      cairo_translate(cr, ox, oy);
      cairo_scale(cr, display_scale, display_scale);
      cairo_mask_surface(cr, svg, 0, 0);
      cairo_restore(cr);
    } else {
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 18.0 * zf);
      cairo_text_extents_t te;
      cairo_text_extents(cr, fallback, &te);
      cairo_move_to(cr, x + (slot_w - te.width) / 2, top_h / 2 + te.height / 2);
      cairo_show_text(cr, fallback);
    }
    // Store position for hit testing
    if (idx == 0) (app.active_pane ? app.r_arrow_back_x : app.arrow_back_x) = x;
    else (app.active_pane ? app.r_arrow_forward_x : app.arrow_forward_x) = x;
    x += slot_w + static_cast<int>(4.0 * zf); // gap-1
  };

  draw_arrow(0, app.arrow_left_svg, "<", app.active_pane ? app.r_arrow_back_hover : app.arrow_back_hover);
  draw_arrow(1, app.arrow_right_svg, ">", app.active_pane ? app.r_arrow_forward_hover : app.arrow_forward_hover);

  int path_left = x;
  int path_margin = static_cast<int>(24.0 * zf); // mx-6

  // ── Right-side controls ──
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * zf);

  auto sort_label = [&]() -> const char* {
    switch (app.cur_tab().sort_field) {
      case SortField::Name:           return "Name";
      case SortField::Size:           return "Size";
      case SortField::Modified:       return "Date";
      case SortField::Type:           return "Type";
      case SortField::FirstModified:  return "First";
      case SortField::LastModified:   return "Last";
      case SortField::Owner:          return "Owner";
      case SortField::Group:          return "Group";
      case SortField::Permissions:    return "Perms";
      case SortField::Extension:      return "Ext";
      case SortField::LinkTarget:     return "Target";
    }
    return "Sort";
  };
  cairo_text_extents_t sort_te;
  cairo_text_extents(cr, sort_label(), &sort_te);
  int sort_label_w = static_cast<int>(sort_te.width);

  // Button sizes (at 100% zoom)
  int gap = static_cast<int>(4.0 * zf); // tighter gap-1
  int right_margin = static_cast<int>(16.0 * zf); // reduced from px-5 for tighter right side


  // View toggle: single button ~40px
  int view_toggle_w = static_cast<int>(40.0 * zf);

  // Sort: px-4(16) + label + ▼(8) + gap-1.5(6)
  int sort_w = static_cast<int>(16.0 * zf) + sort_label_w + static_cast<int>(6.0 * zf) + static_cast<int>(8.0 * zf) + static_cast<int>(16.0 * zf);

  // Gear: px-3(12) + icon(12) + px-3(12)
  int gear_w = static_cast<int>(36.0 * zf);

  // Traffic lights: 3 * 12px + gap-2(8) * 2
  int traffic_w = static_cast<int>(52.0 * zf);

  // Folder-search button (folder + magnifying glass): same size as search
  int folder_search_btn_w = static_cast<int>(40.0 * zf);

  // Search button: same size as view toggle
  int search_btn_w = static_cast<int>(40.0 * zf);

  // Layout from right edge
  int right = content_right - right_margin;
  int traffic_x = right - traffic_w;
  int gear_x = traffic_x - static_cast<int>(8.0 * zf) - gear_w;

  int sort_x = gear_x - gap - sort_w;
  int view_toggle_x = sort_x - gap - view_toggle_w;
  int search_btn_x = view_toggle_x - gap - search_btn_w;
  int folder_search_btn_x = search_btn_x - gap - folder_search_btn_w;

  (app.active_pane ? app.r_search_btn_x : app.search_btn_x) = search_btn_x;
  (app.active_pane ? app.r_search_btn_w : app.search_btn_w) = search_btn_w;
  (app.active_pane ? app.r_folder_search_btn_x : app.folder_search_btn_x) = folder_search_btn_x;
  (app.active_pane ? app.r_folder_search_btn_w : app.folder_search_btn_w) = folder_search_btn_w;
  (app.active_pane ? app.r_view_btn_x : app.view_btn_x) = view_toggle_x;
  (app.active_pane ? app.r_view_btn_w : app.view_btn_w) = view_toggle_w;
  (app.active_pane ? app.r_sort_btn_x : app.sort_btn_x) = sort_x;
  (app.active_pane ? app.r_sort_btn_w : app.sort_btn_w) = sort_w;

  // Path bar fills remaining space
  int path_x = path_left + path_margin;
  int path_w = folder_search_btn_x - gap - path_x;
  if (path_w < 60) path_w = 60;

  int path_h = top_h - static_cast<int>(16.0 * zf);
  int path_y = (top_h - path_h) / 2;

  // ── Gradient bar background + semi-glassy design (inner glassy rim, not outer)
  // The glassy effect is an inner bright frame/rim just inside the bar's rounded edge,
  // with stronger top highlight (Tahoe-Dark style inset white box-shadows).
  {
    int bar_radius = static_cast<int>(12.0 * zf);

    // Base fill: dark translucent (Tahoe headerbar-ish), high see-through so the inner glass shows
    cairo_pattern_t* grad = cairo_pattern_create_linear(0, path_y, 0, path_y + path_h);
    cairo_pattern_add_color_stop_rgba(grad, 0.0, app.surface_r, app.surface_g, app.surface_b, 0.30);
    cairo_pattern_add_color_stop_rgba(grad, 1.0, app.bg_r, app.bg_g, app.bg_b, 0.30);
    cairo_set_source(cr, grad);
    draw_rounded_rect(cr, path_x, path_y, path_w, path_h, bar_radius);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    // Very subtle outer separation only (no strong outer halo/glow — glassy part is inner)
    {
      cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
      cairo_set_line_width(cr, 1.0);
      draw_rounded_rect(cr, path_x + 0.5, path_y + 0.5, path_w - 1.0, path_h - 1.0,
                        static_cast<double>(bar_radius));
      cairo_stroke(cr);
    }

    // Inner semi-glassy bright rim — clean inner glassy frame (no bevel at all).
    // Positioned inset from the outer edge. Stronger top highlight + matching (softer) bottom
    // to close the inner glassy ring around the content.
    {
      cairo_pattern_t* rim = cairo_pattern_create_linear(0, path_y, 0, path_y + path_h);
      cairo_pattern_add_color_stop_rgba(rim, 0.00, app.text_r, app.text_g, app.text_b, 0.28);  // top inner highlight
      cairo_pattern_add_color_stop_rgba(rim, 0.18, app.text_r, app.text_g, app.text_b, 0.14);
      cairo_pattern_add_color_stop_rgba(rim, 0.42, app.text_r, app.text_g, app.text_b, 0.03);
      cairo_pattern_add_color_stop_rgba(rim, 1.00, app.text_r, app.text_g, app.text_b, 0.09);  // small bottom to close the inner frame
      cairo_set_source(cr, rim);
      cairo_set_line_width(cr, 1.35);

      // Inset so the glassy rim is clearly *inside* the bar, forming the inner frame around the content
      double inner_inset = 2.8;
      draw_rounded_rect(cr,
                        path_x + inner_inset,
                        path_y + inner_inset,
                        path_w - inner_inset * 2,
                        path_h - inner_inset * 2,
                        static_cast<double>(bar_radius) - inner_inset + 0.5);
      cairo_stroke(cr);
      cairo_pattern_destroy(rim);
    }

    // Extra top-inner highlight band (pure glass catch)
    {
      cairo_pattern_t* top_hl = cairo_pattern_create_linear(0, path_y + 2, 0, path_y + path_h * 0.4);
      cairo_pattern_add_color_stop_rgba(top_hl, 0.0, app.text_r, app.text_g, app.text_b, 0.09);
      cairo_pattern_add_color_stop_rgba(top_hl, 1.0, app.text_r, app.text_g, app.text_b, 0.0);
      cairo_set_source(cr, top_hl);
      cairo_set_line_width(cr, 0.7);
      double hl_inset = 3.8;
      draw_rounded_rect(cr,
                        path_x + hl_inset,
                        path_y + hl_inset,
                        path_w - hl_inset * 2,
                        path_h - hl_inset * 2,
                        static_cast<double>(bar_radius) - hl_inset + 1);
      cairo_stroke(cr);
      cairo_pattern_destroy(top_hl);
    }

    // Bottom-inner highlight band (symmetric to top, softer) to complete the inner glassy frame
    {
      cairo_pattern_t* bottom_hl = cairo_pattern_create_linear(0, path_y + path_h * 0.55, 0, path_y + path_h - 2);
      cairo_pattern_add_color_stop_rgba(bottom_hl, 0.0, app.text_r, app.text_g, app.text_b, 0.0);
      cairo_pattern_add_color_stop_rgba(bottom_hl, 1.0, app.text_r, app.text_g, app.text_b, 0.07);  // softer than top
      cairo_set_source(cr, bottom_hl);
      cairo_set_line_width(cr, 0.7);
      double hl_inset = 3.8;
      draw_rounded_rect(cr,
                        path_x + hl_inset,
                        path_y + hl_inset,
                        path_w - hl_inset * 2,
                        path_h - hl_inset * 2,
                        static_cast<double>(bar_radius) - hl_inset + 1);
      cairo_stroke(cr);
      cairo_pattern_destroy(bottom_hl);
    }
  }

  // ── Three-dot menu on the far right ──
  int dots_btn_w = static_cast<int>(24.0 * zf);
  int dots_x = path_x + path_w - dots_btn_w + static_cast<int>(2.0 * zf);
  int dots_y = path_y;
  (app.active_pane ? app.r_dots_btn_x : app.dots_btn_x) = dots_x;
  (app.active_pane ? app.r_dots_btn_y : app.dots_btn_y) = dots_y;
  (app.active_pane ? app.r_dots_btn_w : app.dots_btn_w) = dots_btn_w;
  (app.active_pane ? app.r_dots_btn_h : app.dots_btn_h) = path_h;
  // Three filled circles (⋮) with tight spacing
  {
    double dot_r = 1.1 * zf;
    double gap = 2.8 * zf;
    double cx = dots_x + dots_btn_w / 2.0;
    double cy0 = path_y + path_h / 2.0 - gap - dot_r;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                            (app.active_pane ? app.r_dots_btn_hover : app.dots_btn_hover) ? 0.85 : 0.5);
    for (int i = 0; i < 3; ++i) {
      cairo_arc(cr, cx, cy0 + i * (2.0 * dot_r + gap), dot_r, 0.0, 2.0 * M_PI);
      cairo_fill(cr);
    }
  }

  // ── Path content (house icon + breadcrumbs or editing) ──
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * zf);

  int text_x = path_x + static_cast<int>(12.0 * zf);
  int text_y = top_h / 2 + static_cast<int>(4.0 * zf);

  // Draw house icon
  int icon_sz = static_cast<int>(12.0 * zf);
  int icon_y = (top_h - icon_sz) / 2;
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.75);
  draw_house_icon(cr, text_x, icon_y, icon_sz);

  int path_text_x = text_x + icon_sz + static_cast<int>(12.0 * zf); // gap-3
  int path_text_w = path_w - (path_text_x - path_x) - dots_btn_w - static_cast<int>(16.0 * zf);

  if ((app.active_pane ? app.r_search_active : app.search_active) || (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active)) {
    // ── Search bar ──
    int search_left = path_text_x;
    int search_right = path_x + path_w - dots_btn_w - static_cast<int>(8.0 * zf);
    int search_w = search_right - search_left;
    int search_icon_size = static_cast<int>(14.0 * zf);
    int search_icon_x = search_left;
    int search_text_left = search_left + search_icon_size + static_cast<int>(8.0 * zf);
    (app.active_pane ? app.r_search_bar_x : app.search_bar_x) = search_left;
    (app.active_pane ? app.r_search_bar_w : app.search_bar_w) = search_w;

    // Draw magnifying glass icon (SVG or fallback text)
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.5);
    if (app.search_svg) {
      double svg_w = static_cast<double>(cairo_image_surface_get_width(app.search_svg));
      double svg_h = static_cast<double>(cairo_image_surface_get_height(app.search_svg));
      int sz = search_icon_size;
      int ox = search_icon_x;
      int oy = (top_h - sz) / 2;
      double display_scale = sz / std::max(svg_w, svg_h);
      cairo_save(cr);
      cairo_rectangle(cr, ox, oy, sz, sz);
      cairo_clip(cr);
      cairo_translate(cr, ox, oy);
      cairo_scale(cr, display_scale, display_scale);
      cairo_mask_surface(cr, app.search_svg, 0, 0);
      cairo_restore(cr);
    } else {
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, search_icon_size * 0.8);
      cairo_move_to(cr, search_icon_x + 2.0, (top_h + search_icon_size * 0.4) / 2);
      cairo_show_text(cr, "\u2315");
    }

    // Search text
    std::string display = (app.active_pane ? app.r_search_query : app.search_query);
    bool has_text = !(app.active_pane ? app.r_search_query : app.search_query).empty();
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0 * zf);

    if (has_text) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    } else {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.35);
      cairo_move_to(cr, search_text_left, text_y);
      cairo_show_text(cr, (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active) ? "Recursive search..." : "Search...");
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    }
    cairo_move_to(cr, search_text_left, text_y);
    cairo_show_text(cr, display.c_str());

    // Cursor (text-height, centered)
    {
      std::string before = (app.active_pane ? app.r_search_query : app.search_query).substr(0, static_cast<std::size_t>(app.active_pane ? app.r_search_cursor : app.search_cursor));
      cairo_text_extents_t cur_te;
      cairo_text_extents(cr, before.c_str(), &cur_te);
      int cursor_x = search_text_left + static_cast<int>(cur_te.width);
      int cursor_h = static_cast<int>(14.0 * zf);
      int cursor_y = (top_h - cursor_h) / 2;
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.8);
      cairo_rectangle(cr, cursor_x, cursor_y, 1, cursor_h);
      cairo_fill(cr);
    }

    // ── Filter button + clear button (right-to-left layout) ──
    int right_cursor = search_right;
    int btn_gap = static_cast<int>(6.0 * zf);

    // Clear button (×)
    if (has_text) {
      std::string clear_str = "×";
      cairo_text_extents_t clear_te;
      cairo_text_extents(cr, clear_str.c_str(), &clear_te);
      int clear_w = static_cast<int>(clear_te.width) + static_cast<int>(12.0 * zf);
      right_cursor -= clear_w;
      (app.active_pane ? app.r_search_clear_x : app.search_clear_x) = right_cursor;
      (app.active_pane ? app.r_search_clear_w : app.search_clear_w) = clear_w;
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.5);
      cairo_move_to(cr, right_cursor + (clear_w - clear_te.width) / 2, text_y);
      cairo_show_text(cr, clear_str.c_str());
    } else {
      (app.active_pane ? app.r_search_clear_x : app.search_clear_x) = 0;
      (app.active_pane ? app.r_search_clear_w : app.search_clear_w) = 0;
    }

    // Single Filter button (glassy outline style like location bar)
    {
      bool any_active = (app.active_pane ? app.r_filter_type_idx : app.filter_type_idx) > 0 || (app.active_pane ? app.r_filter_size_idx : app.filter_size_idx) > 0 || (app.active_pane ? app.r_filter_date_idx : app.filter_date_idx) > 0;
      std::string label = any_active ? "Filtered" : "Filter";
      cairo_text_extents_t te;
      cairo_text_extents(cr, label.c_str(), &te);
      int bw = static_cast<int>(te.width) + static_cast<int>(20.0 * zf);
      int bh = static_cast<int>(24.0 * zf);
      int by = (top_h - bh) / 2;
      right_cursor -= (btn_gap + bw);
      (app.active_pane ? app.r_filter_btn_x : app.filter_btn_x) = right_cursor;
      (app.active_pane ? app.r_filter_btn_w : app.filter_btn_w) = bw;
      bool hv = app.active_pane ? app.r_filter_btn_hover : app.filter_btn_hover;
      int btn_r = static_cast<int>(6.0 * zf);

      // Glassy gradient background
      cairo_pattern_t* grad = cairo_pattern_create_linear(0, by, 0, by + bh);
      cairo_pattern_add_color_stop_rgba(grad, 0.0, app.surface_r, app.surface_g, app.surface_b, any_active ? 0.45 : 0.25);
      cairo_pattern_add_color_stop_rgba(grad, 1.0, app.bg_r, app.bg_g, app.bg_b, any_active ? 0.45 : 0.25);
      cairo_set_source(cr, grad);
      draw_rounded_rect(cr, right_cursor, by, bw, bh, btn_r);
      cairo_fill(cr);
      cairo_pattern_destroy(grad);

      // Outer dark border
      cairo_set_source_rgba(cr, 0, 0, 0, hv ? 0.20 : 0.12);
      cairo_set_line_width(cr, 1.0);
      draw_rounded_rect(cr, right_cursor + 0.5, by + 0.5, bw - 1.0, bh - 1.0, btn_r);
      cairo_stroke(cr);

      // Inner glassy bright rim
      cairo_pattern_t* rim = cairo_pattern_create_linear(0, by, 0, by + bh);
      cairo_pattern_add_color_stop_rgba(rim, 0.00, app.text_r, app.text_g, app.text_b, hv ? 0.30 : any_active ? 0.25 : 0.18);
      cairo_pattern_add_color_stop_rgba(rim, 0.30, app.text_r, app.text_g, app.text_b, hv ? 0.18 : any_active ? 0.14 : 0.08);
      cairo_pattern_add_color_stop_rgba(rim, 1.00, app.text_r, app.text_g, app.text_b, hv ? 0.14 : any_active ? 0.10 : 0.05);
      cairo_set_source(cr, rim);
      cairo_set_line_width(cr, 1.0);
      double inset = 2.0;
      draw_rounded_rect(cr, right_cursor + inset, by + inset, bw - inset * 2, bh - inset * 2,
                        static_cast<double>(btn_r) - inset + 0.5);
      cairo_stroke(cr);
      cairo_pattern_destroy(rim);

      // Text
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, any_active ? 1.0 : hv ? 0.9 : 0.7);
      cairo_move_to(cr, right_cursor + (bw - te.width) / 2, text_y);
      cairo_show_text(cr, label.c_str());
    }

    // ── Query-mode segment, case toggle, lock (left of Filter) ──
    {
      auto& dw_mode = app.active_pane ? app.r_search_mode : app.search_mode;
      auto& dw_case = app.active_pane ? app.r_search_case_sensitive : app.search_case_sensitive;
      auto& dw_lock = app.active_pane ? app.r_search_locked : app.search_locked;
      auto& dw_mode_x = app.active_pane ? app.r_search_mode_x : app.search_mode_x;
      auto& dw_mode_w = app.active_pane ? app.r_search_mode_w : app.search_mode_w;
      auto& dw_case_x = app.active_pane ? app.r_search_case_x : app.search_case_x;
      auto& dw_case_w = app.active_pane ? app.r_search_case_w : app.search_case_w;
      auto& dw_lock_x = app.active_pane ? app.r_search_lock_x : app.search_lock_x;
      auto& dw_lock_w = app.active_pane ? app.r_search_lock_w : app.search_lock_w;

      int bh2 = static_cast<int>(22.0 * zf);
      int by2 = (top_h - bh2) / 2;
      int seg_w = static_cast<int>(34.0 * zf);
      int seg_count = 4;
      int seg_total = seg_w * seg_count;

      // Lock button (rightmost of this group)
      dw_lock_w = static_cast<int>(26.0 * zf);
      right_cursor -= (btn_gap + dw_lock_w);
      dw_lock_x = right_cursor;
      {
        bool hv2 = app.active_pane ? app.r_search_lock_hover : app.search_lock_hover;
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                              dw_lock ? 0.95 : hv2 ? 0.6 : 0.35);
        double cx = right_cursor + dw_lock_w / 2.0;
        double cy = top_h / 2.0;
        double s = 5.0 * zf;
        cairo_set_line_width(cr, 1.4);
        cairo_rectangle(cr, cx - s * 0.75, cy - s * 0.1, s * 1.5, s * 1.1);
        if (dw_lock) cairo_fill(cr); else cairo_stroke(cr);
        cairo_arc(cr, cx, cy - s * 0.1, s * 0.45, M_PI, 2 * M_PI);
        cairo_stroke(cr);
      }

      // Case toggle ("Aa")
      dw_case_w = static_cast<int>(28.0 * zf);
      right_cursor -= dw_case_w;
      dw_case_x = right_cursor;
      {
        bool hv2 = app.active_pane ? app.r_search_case_hover : app.search_case_hover;
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                              dw_case ? 0.95 : hv2 ? 0.6 : 0.35);
        cairo_text_extents_t te2;
        cairo_text_extents(cr, "Aa", &te2);
        cairo_move_to(cr, right_cursor + (dw_case_w - te2.width) / 2, text_y);
        cairo_show_text(cr, "Aa");
      }
      right_cursor -= btn_gap;

      // Mode segment control
      dw_mode_w = seg_total;
      right_cursor -= seg_total;
      dw_mode_x = right_cursor;
      {
        static constexpr const char* kSegLabels[] = {"abc", "*", ".*", "txt"};
        bool hv2 = app.active_pane ? app.r_search_mode_hover : app.search_mode_hover;
        int hov_btn = app.active_pane ? app.r_search_mode_hover_btn : app.search_mode_hover_btn;
        for (int si = 0; si < seg_count; ++si) {
          bool sel = dw_mode == si;
          int sx = right_cursor + si * seg_w;
          if (sel) {
            cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.28);
            draw_rounded_rect(cr, sx + 1, by2, seg_w - 2, bh2, 5);
            cairo_fill(cr);
          } else if (hv2 && si == hov_btn) {
            cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.10);
            draw_rounded_rect(cr, sx + 1, by2, seg_w - 2, bh2, 5);
            cairo_fill(cr);
          }
          cairo_text_extents_t te2;
          cairo_text_extents(cr, kSegLabels[si], &te2);
          cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                                sel ? 1.0 : 0.45);
          cairo_move_to(cr, sx + (seg_w - te2.width) / 2, text_y);
          cairo_show_text(cr, kSegLabels[si]);
        }
        cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
        cairo_set_line_width(cr, 1);
        draw_rounded_rect(cr, right_cursor + 0.5, by2 + 0.5, seg_total - 1, bh2 - 1, 5);
        cairo_stroke(cr);
      }
      right_cursor -= btn_gap;

      // Red invalid underline for malformed regex
      bool regex_bad = !(app.active_pane ? app.r_search_regex_valid : app.search_regex_valid);
      if (regex_bad && has_text) {
        cairo_set_source_rgba(cr, 0.9, 0.25, 0.25, 0.9);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, search_text_left, text_y + static_cast<int>(4.0 * zf));
        cairo_line_to(cr, search_text_left + static_cast<int>(60.0 * zf),
                      text_y + static_cast<int>(4.0 * zf));
        cairo_stroke(cr);
      }
    }
  } else if (app.active_pane ? app.r_path_editing : app.path_editing) {
    // ── Editable location bar ──
    auto& dw_pe_buf = app.active_pane ? app.r_path_edit_buf : app.path_edit_buf;
    auto& dw_pe_cursor = app.active_pane ? app.r_path_edit_cursor : app.path_edit_cursor;
    auto& dw_pe_sel_start = app.active_pane ? app.r_path_edit_sel_start : app.path_edit_sel_start;
    auto& dw_pe_sel_end = app.active_pane ? app.r_path_edit_sel_end : app.path_edit_sel_end;
    cairo_text_extents_t te;
    cairo_text_extents(cr, dw_pe_buf.c_str(), &te);
    std::string display = dw_pe_buf;
    int scroll_offset = 0;
    if (te.width > path_text_w) {
      int keep = static_cast<int>(display.size()) * path_text_w /
                 std::max(1, static_cast<int>(te.width));
      if (keep > 3 && keep < static_cast<int>(display.size())) {
        int trim = static_cast<int>(display.size()) - keep + 3;
        scroll_offset = trim;
        display = "..." + display.substr(static_cast<std::size_t>(trim));
      }
    }

    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    double text_h = fe.ascent + fe.descent;
    double text_top = text_y - fe.ascent;

    if (dw_pe_sel_start >= 0 && dw_pe_sel_start != dw_pe_sel_end) {
      int sel_a = std::min(dw_pe_sel_start, dw_pe_sel_end);
      int sel_b = std::max(dw_pe_sel_start, dw_pe_sel_end);
      int disp_sel_a = std::max(0, sel_a - scroll_offset) + (scroll_offset > 0 ? 3 : 0);
      int disp_sel_b = std::max(0, sel_b - scroll_offset) + (scroll_offset > 0 ? 3 : 0);
      disp_sel_a = std::min(disp_sel_a, static_cast<int>(display.size()));
      disp_sel_b = std::min(disp_sel_b, static_cast<int>(display.size()));
      if (disp_sel_a < disp_sel_b) {
        cairo_text_extents_t sel_te;
        std::string before_sel = display.substr(0, disp_sel_a);
        std::string sel_text = display.substr(disp_sel_a, disp_sel_b - disp_sel_a);
        cairo_text_extents(cr, before_sel.c_str(), &sel_te);
        double sel_x = path_text_x + sel_te.width;
        cairo_text_extents(cr, sel_text.c_str(), &sel_te);
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.35);
        cairo_rectangle(cr, sel_x, text_top, sel_te.width, text_h);
        cairo_fill(cr);
      }
    }

    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, path_text_x, text_y);
    cairo_show_text(cr, display.c_str());

    if (dw_pe_sel_start < 0 || dw_pe_sel_start == dw_pe_sel_end) {
      cairo_text_extents_t cur_te;
      int disp_cursor = std::max(0, dw_pe_cursor - scroll_offset) +
                        (scroll_offset > 0 ? 3 : 0);
      disp_cursor = std::min(disp_cursor, static_cast<int>(display.size()));
      std::string before = display.substr(0, static_cast<std::size_t>(disp_cursor));
      cairo_text_extents(cr, before.c_str(), &cur_te);
      int cursor_x = path_text_x + static_cast<int>(cur_te.width);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.8);
      cairo_rectangle(cr, cursor_x, text_top, 1, text_h);
      cairo_fill(cr);
    }
  } else {
    // ── Simple location label (single friendly name + house, matching Design.png aesthetic) ──
    (app.active_pane ? app.r_breadcrumbs : app.breadcrumbs).clear();
    (app.active_pane ? app.r_breadcrumb_hover : app.breadcrumb_hover) = -1;

    // Compute friendly display name (Home / Pictures / current folder basename etc.)
    std::string label;
    std::string cur = app.cur_tab().current_path;
    if (!cur.empty() && cur[0] == '/') {
      std::string h = home_dir();
      if (cur == h || cur == h + "/") {
        label = "Home";
      } else {
        bool found = false;
        for (const auto& loc : app.sidebar_locations) {
          if (!loc.path.empty() && loc.path == cur) {
            label = loc.label;
            found = true;
            break;
          }
        }
        if (!found) {
          auto pos = cur.rfind('/');
          label = (pos != std::string::npos && pos + 1 < cur.size()) ? cur.substr(pos + 1) : cur;
          if (label.empty()) label = "/";
        }
      }
    } else {
      label = cur.empty() ? "Home" : cur;
    }

    cairo_set_font_size(cr, 13.0 * zf);

    // Measure + elide if needed
    cairo_text_extents_t label_te;
    std::string display_label = label;
    cairo_text_extents(cr, display_label.c_str(), &label_te);
    if (label_te.x_advance > path_text_w - 4) {
      while (!display_label.empty() && label_te.x_advance > path_text_w - 16) {
        display_label.pop_back();
        cairo_text_extents(cr, (display_label + "…").c_str(), &label_te);
      }
      display_label += "…";
      cairo_text_extents(cr, display_label.c_str(), &label_te);
    }
    int label_w = static_cast<int>(label_te.x_advance + 4.0 * zf);
    if (label_w > path_text_w) label_w = path_text_w;

    bool label_hovered = ((app.active_pane ? app.r_breadcrumb_hover : app.breadcrumb_hover) == 0);
    if (label_hovered) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
      draw_rounded_rect(cr, path_text_x - 2, path_y + 2, label_w + 4, path_h - 4,
                        static_cast<int>(5.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }

    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, label_hovered ? 1.0 : 0.92);
    cairo_move_to(cr, path_text_x, text_y);
    cairo_show_text(cr, display_label.c_str());

    // One breadcrumb entry for hit testing / hover (clicking it is a no-op; empty space in the bar enters edit)
    BreadcrumbSegment seg;
    seg.label = display_label;
    seg.path = cur;
    seg.x = path_text_x;
    seg.w = label_w;
    (app.active_pane ? app.r_breadcrumbs : app.breadcrumbs).push_back(seg);
  }

  // ── View-mode toggle (cycles List→Grid→Compact→Tree→List) ──
  {
    const char* icon = "\u25A6";
    switch (app.cur_tab().view_mode) {
      case ViewMode::List:    icon = "\u25A6"; break; // grid icon (next: Grid)
      case ViewMode::Grid:    icon = "\u2261"; break; // list icon (next: Compact)
      case ViewMode::Compact: icon = "\u25B3"; break; // tree icon (next: Tree)
      case ViewMode::Tree:    icon = "\u25A3"; break; // list icon (next: List)
      default:                icon = "\u25A6"; break;
    }
    cairo_text_extents_t te;
    cairo_text_extents(cr, icon, &te);
    cairo_move_to(cr, view_toggle_x + (view_toggle_w - te.width) / 2,
                   path_y + path_h / 2 + te.height / 2);
    cairo_show_text(cr, icon);
  }

  // ── Folder-search button (folder + magnifying glass) ──
  {
    bool hv = app.active_pane ? app.r_folder_search_btn_hover : app.folder_search_btn_hover;
    bool active = (app.active_pane ? app.r_search_active : app.search_active);
    if (hv || active) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, active ? 0.25 : 0.08);
      draw_rounded_rect(cr, folder_search_btn_x, path_y, folder_search_btn_w, path_h,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, active ? 1.0 : hv ? 0.9 : 0.6);
    int sz = static_cast<int>(16.0 * zf);
    int ox = folder_search_btn_x + (folder_search_btn_w - sz) / 2;
    int oy = (top_h - sz) / 2;
    cairo_surface_t* fs_svg = app.folder_search_svg;
    if (fs_svg) {
      double svg_w = static_cast<double>(cairo_image_surface_get_width(fs_svg));
      double svg_h = static_cast<double>(cairo_image_surface_get_height(fs_svg));
      double display_scale = sz / std::max(svg_w, svg_h);
      cairo_save(cr);
      cairo_rectangle(cr, ox, oy, sz, sz);
      cairo_clip(cr);
      cairo_translate(cr, ox, oy);
      cairo_scale(cr, display_scale, display_scale);
      cairo_mask_surface(cr, fs_svg, 0, 0);
      cairo_restore(cr);
    } else {
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 13.0 * zf);
      cairo_move_to(cr, folder_search_btn_x + (folder_search_btn_w - 6.0 * zf) / 2,
                     top_h / 2 + static_cast<int>(4.0 * zf));
      cairo_show_text(cr, "F");
    }
  }

  // ── Search button (magnifying glass) ──
  {
    bool hv = app.active_pane ? app.r_search_btn_hover : app.search_btn_hover;
    bool active = (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active);
    if (hv || active) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, active ? 0.25 : 0.08);
      draw_rounded_rect(cr, search_btn_x, path_y, search_btn_w, path_h,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, active ? 1.0 : hv ? 0.9 : 0.6);
    if (app.search_svg) {
      double svg_w = static_cast<double>(cairo_image_surface_get_width(app.search_svg));
      double svg_h = static_cast<double>(cairo_image_surface_get_height(app.search_svg));
      int sz = static_cast<int>(14.0 * zf);
      int ox = search_btn_x + (search_btn_w - sz) / 2;
      int oy = (top_h - sz) / 2;
      double display_scale = sz / std::max(svg_w, svg_h);
      cairo_save(cr);
      cairo_rectangle(cr, ox, oy, sz, sz);
      cairo_clip(cr);
      cairo_translate(cr, ox, oy);
      cairo_scale(cr, display_scale, display_scale);
      cairo_mask_surface(cr, app.search_svg, 0, 0);
      cairo_restore(cr);
    } else {
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 13.0 * zf);
      cairo_move_to(cr, search_btn_x + (search_btn_w - 6.0 * zf) / 2,
                     top_h / 2 + static_cast<int>(4.0 * zf));
      cairo_show_text(cr, "S");
    }
  }

  // ── Sort button ──
  {
    bool hv = app.active_pane ? app.r_sort_btn_hover : app.sort_btn_hover;
    if (hv) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, sort_x, path_y, sort_w, path_h,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0 * zf);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, sort_x + static_cast<int>(16.0 * zf),
                   top_h / 2 + static_cast<int>(4.0 * zf));
    cairo_show_text(cr, sort_label());
  }

  // ── Settings gear button ──
  {
    bool hv = app.active_pane ? app.r_settings_btn_hover : app.settings_btn_hover;
    if (hv) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, gear_x, path_y, gear_w, path_h,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 16.0 * zf);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, gear_x + (gear_w - 14.0 * zf) / 2,
                   top_h / 2 + static_cast<int>(5.0 * zf));
    cairo_show_text(cr, "\u2699");
  }

  } // end split-view guard (skip full bar when split_view && global)

  // ── macOS-style traffic lights (always right) ──
  if (pane_w == 0) {
    // Recalculate traffic_x since it's inside the split-view guard above
    int right_margin = static_cast<int>(16.0 * zf);
    int traffic_w = static_cast<int>(52.0 * zf);
    int right = content_right - right_margin;
    int traffic_x = right - traffic_w;
    int light_d = static_cast<int>(12.0 * zf);
    int light_gap = static_cast<int>(8.0 * zf); // gap-2
    int light_y = (top_h - light_d) / 2;

    auto draw_light = [&](int lx, bool hover, double r, double g, double b) {
      double rad = light_d / 2.0;
      if (hover) {
        cairo_set_source_rgba(cr, r, g, b, 0.4);
        cairo_arc(cr, lx + rad, light_y + rad, rad + 2, 0, 2 * M_PI);
        cairo_fill(cr);
      }
      cairo_set_source_rgba(cr, r, g, b, 1.0);
      cairo_arc(cr, lx + rad, light_y + rad, rad, 0, 2 * M_PI);
      cairo_fill(cr);
    };

    // Maximize (green)
    draw_light(traffic_x, app.win_btn_max_hover, 0.18, 0.80, 0.44);
    // Minimize (yellow)
    draw_light(traffic_x + light_d + light_gap, app.win_btn_min_hover, 0.95, 0.76, 0.04);
    // Close (red)
    draw_light(traffic_x + (light_d + light_gap) * 2, app.win_btn_close_hover, 0.91, 0.30, 0.24);

    // Store window control positions for hit testing
    app.win_btn_x = traffic_x;
    app.win_btn_w = light_d + light_gap;
  }
}

// ── filter dropdown with expandable sections ─────────────────────

static constexpr int kFilterHdrH = 28;
static constexpr int kFilterItemH = 24;
static constexpr int kFilterPD = 6;
static constexpr int kFilterSep = 4;
static constexpr int kFilterW = 180;

void draw_filter_dropdown(AppState& app, cairo_t* cr, int section) {
  int top_h = app.top_bar_height;

  auto items_for = [](int s) { return s == 1 ? 13 : s == 2 ? 7 : 5; };
  int h = kFilterPD;
  for (int s = 1; s <= 3; ++s) {
    h += kFilterHdrH + kFilterSep;
    if (section == s) h += items_for(s) * kFilterItemH;
  }
  h += kFilterPD;

  int menu_x = app.filter_btn_x + app.filter_btn_w - kFilterW;
  if (menu_x < 0) menu_x = 0;
  int menu_y = top_h;

  (app.active_pane ? app.r_filter_dropdown_x : app.filter_dropdown_x) = menu_x;
  (app.active_pane ? app.r_filter_dropdown_y : app.filter_dropdown_y) = menu_y;
  (app.active_pane ? app.r_filter_dropdown_w : app.filter_dropdown_w) = kFilterW;
  (app.active_pane ? app.r_filter_dropdown_h : app.filter_dropdown_h) = h;

  for (int s = 3; s >= 0; --s) {
    double a = 0.08 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, menu_x + s * 2, menu_y + s * 2, kFilterW, h, 6);
    cairo_fill(cr);
  }
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 1.0);
  draw_rounded_rect(cr, menu_x, menu_y, kFilterW, h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, menu_x + 0.5, menu_y + 0.5, kFilterW - 1, h - 1, 5.5);
  cairo_stroke(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);

  struct SecInfo { const char* name; const char* const* labels; int count; int cur_idx; };
  SecInfo sections[3] = {
    {"Type", kFilterTypeLabels, 13, app.active_pane ? app.r_filter_type_idx : app.filter_type_idx},
    {"Size", kFilterSizeLabels, 7, app.active_pane ? app.r_filter_size_idx : app.filter_size_idx},
    {"Date", kFilterDateLabels, 5, app.active_pane ? app.r_filter_date_idx : app.filter_date_idx},
  };

  int y = menu_y + kFilterPD;
  int glob_idx = 0;
  for (int si = 0; si < 3; ++si) {
    int sec_num = si + 1;
    bool expanded = (section == sec_num);
    auto& info = sections[si];

    // Header
      bool hdr_hit = (glob_idx == (app.active_pane ? app.r_filter_dropdown_hover : app.filter_dropdown_hover));
    if (hdr_hit) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      draw_rounded_rect(cr, menu_x + 4, y, kFilterW - 8, kFilterHdrH, 4);
      cairo_fill(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.7);
    cairo_move_to(cr, menu_x + 10, y + kFilterHdrH / 2 + 4);
    cairo_show_text(cr, expanded ? "\u25BC " : "\u25B6 ");
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_show_text(cr, info.name);
    if (!expanded && info.cur_idx > 0 && info.cur_idx < info.count) {
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.6);
      cairo_move_to(cr, menu_x + kFilterW - 12 - kFilterItemH, y + kFilterHdrH / 2 + 4);
      cairo_show_text(cr, info.labels[info.cur_idx]);
    }
    ++glob_idx;
    y += kFilterHdrH;

    // Items
    if (expanded) {
      for (int i = 0; i < info.count; ++i) {
        bool ihover = (glob_idx == (app.active_pane ? app.r_filter_dropdown_hover : app.filter_dropdown_hover));
        bool iactive = (i == info.cur_idx);
        if (ihover) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
          draw_rounded_rect(cr, menu_x + 8, y, kFilterW - 16, kFilterItemH, 4);
          cairo_fill(cr);
        }
        int tx = menu_x + 16;
        if (iactive) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
          cairo_move_to(cr, tx, y + kFilterItemH / 2 + 4);
          cairo_show_text(cr, "✓");
          tx += 14;
        }
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, iactive ? 1.0 : 0.85);
        cairo_move_to(cr, tx + 4, y + kFilterItemH / 2 + 4);
        cairo_show_text(cr, info.labels[i]);
        ++glob_idx;
        y += kFilterItemH;
      }
    }

    y += kFilterSep;
  }
}

// ── hover preview popup ──────────────────────────────────────────

static bool is_pdf_preview(const AppState& app) {
  return is_pdf_extension(app.preview_path);
}

// Rich tooltip metadata card (folders / entries without a live preview).
// Drawn into the tooltip subsurface at (tooltip_x, tooltip_y).
void draw_tooltip_card(AppState& app, cairo_t* cr) {
  int px = app.tooltip_x;
  int py = app.tooltip_y;
  int pw = app.tooltip_w;
  int ph = app.tooltip_h;
  int radius = 8;

  // Frame (same style as the hover preview popup)
  for (int s = 3; s >= 0; --s) {
    double a = 0.08 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, px + s * 2, py + s * 2, pw, ph, radius);
    cairo_fill(cr);
  }
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 1.0);
  draw_rounded_rect(cr, px, py, pw, ph, radius);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, px + 0.5, py + 0.5, pw - 1, ph - 1, radius - 0.5);
  cairo_stroke(cr);

  int pad = 12;
  int y = py + pad + 6;

  // Title
  std::string title = app.tooltip_title;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 12);
  cairo_text_extents_t te;
  cairo_text_extents(cr, title.c_str(), &te);
  if (te.width > pw - pad * 2) {
    while (title.size() > 4 &&
           (cairo_text_extents(cr, (title + "...").c_str(), &te), te.width > pw - pad * 2)) {
      title.pop_back();
    }
    title += "...";
    cairo_text_extents(cr, title.c_str(), &te);
  }
  cairo_set_source_rgba(cr, app.text_r * 0.9, app.text_g * 0.9, app.text_b * 0.9, 0.95);
  cairo_move_to(cr, px + pad, y);
  cairo_show_text(cr, title.c_str());
  y += 24;

  // Metadata rows
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 11);
  for (const auto& row : app.tooltip_rows) {
    std::string line = row;
    cairo_text_extents(cr, line.c_str(), &te);
    if (te.width > pw - pad * 2) {
      while (line.size() > 4 &&
             (cairo_text_extents(cr, (line + "...").c_str(), &te), te.width > pw - pad * 2)) {
        line.pop_back();
      }
      line += "...";
    }
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 0.95);
    cairo_move_to(cr, px + pad, y);
    cairo_show_text(cr, line.c_str());
    y += 20;
  }
}

// One-time warm-up of the cairo "toy" font stack (fontconfig init + font
// load for the families popups/tooltips use) and Pango, which otherwise
// cost 500-700ms on their first real use and stall the frame that happens
// to trigger them. Call once at startup.
void warmup_text_rendering() {
  cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
  cairo_t* cr = cairo_create(s);
  const char* fams[] = {"Sans", "monospace"};
  for (const char* fam : fams) {
    for (int weight : {CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_WEIGHT_BOLD}) {
      cairo_select_font_face(cr, fam, CAIRO_FONT_SLANT_NORMAL,
                             static_cast<cairo_font_weight_t>(weight));
      cairo_set_font_size(cr, 12);
      cairo_text_extents_t te;
      cairo_text_extents(cr, "Ag", &te);
      cairo_move_to(cr, 2, 10);
      cairo_show_text(cr, "Ag");
    }
  }
  // Sidebar/labels render via the toy API at these sizes; warming the glyph
  // caches here keeps the cost off the first real paint (~2-3 ms saved).
  {
    static const char* kUiStrings[] = {
        "My Computer", "Home", "Desktop", "Documents", "Downloads",
        "Pictures", "Music", "Videos", "Trash", "Root", "File System",
    };
    // Sidebar/toolbar render at zoom-scaled sizes (10..16 px typical); warm
    // every candidate size so the real paint never rasterizes glyphs cold.
    for (int weight : {CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_WEIGHT_BOLD}) {
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             static_cast<cairo_font_weight_t>(weight));
      for (double sz : {11.0, 13.0, 14.0, 16.0}) {
        cairo_set_font_size(cr, sz);
        cairo_move_to(cr, 0, sz);
        for (const char* str : kUiStrings) cairo_show_text(cr, str);
      }
    }
  }
  cairo_surface_flush(s);
  cairo_destroy(cr);
  cairo_surface_destroy(s);

#if defined(HAVE_PANGO)
  cairo_surface_t* ps = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
  cairo_t* pcr = cairo_create(ps);
  if (PangoLayout* pl = pango_cairo_create_layout(pcr)) {
    PangoFontDescription* d = pango_font_description_new();
    pango_font_description_set_family(d, "Sans");
    pango_font_description_set_absolute_size(d, 13 * PANGO_SCALE);
    pango_layout_set_font_description(pl, d);
    pango_layout_set_text(pl, "Ag", -1);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(pl, &w, &h);
    pango_cairo_show_layout(pcr, pl);
    pango_font_description_free(d);
    g_object_unref(pl);
  }
  cairo_surface_flush(ps);
  cairo_destroy(pcr);
  cairo_surface_destroy(ps);
#endif
}

void draw_hover_preview(AppState& app, cairo_t* cr) {
  int px = app.preview_x;
  int py = app.preview_y;
  int pw = app.preview_w;
  int ph = app.preview_h;
  int radius = 8;

  bool pdf = is_pdf_preview(app);
  bool space_mode = (app.preview_mode == AppState::PreviewMode::Space);

  // Entry lookup (before background so image_preview is known)
  int vi = app.preview_entry_idx;
  std::string name;
  std::string info;
  FileType type = FileType::File;
  if (vi >= 0 && vi < static_cast<int>(app.cur_tab().visible_entries.size())) {
    int ri = app.cur_tab().visible_entries[vi];
    if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size())) {
      const auto& entry = app.cur_tab().entries[ri];
      name = entry.name;
      type = entry.type;
      auto fmt_size = [](uint64_t bytes) -> std::string {
        if (bytes < 1024) return std::to_string(bytes) + " B";
        if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
        if (bytes < 1024 * 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
        return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
      };
      info = fmt_size(entry.size);
      const char* type_names[] = {"Folder", "Image", "Audio", "Video", "Text", "Markdown",
                                   "Code", "Document", "Font", "Archive", "Executable", "Web", "File"};
      int ti = static_cast<int>(type);
      if (ti >= 0 && ti < 13) {
        info += "  \u00B7  ";
        info += type_names[ti];
      }
    }
  }

  bool fill_preview = ((type == FileType::Image || type == FileType::Video) &&
                       app.preview_thumb);

  // ── Context‑menu style popup (solid fill + outline) ──
  double frame_alpha = app.preview_opacity_pct / 100.0;
  for (int s = 3; s >= 0; --s) {
    double a = 0.08 * (1.0 - s / 4.0) * frame_alpha;
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, px + s * 2, py + s * 2, pw, ph, radius);
    cairo_fill(cr);
  }
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, frame_alpha);
  draw_rounded_rect(cr, px, py, pw, ph, radius);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25 * frame_alpha);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, px + 0.5, py + 0.5, pw - 1, ph - 1, radius - 0.5);
  cairo_stroke(cr);

  bool has_text = !app.preview_text.empty();
  int content_top = py + 10;
  int content_bottom = py + ph - 50;

  if (has_text) {
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.85);
    double line_h = 16;
    int lines_max = static_cast<int>((content_bottom - content_top) / line_h);
    std::string text = app.preview_text;
    for (auto& ch : text) {
      if (ch < 32 && ch != '\n' && ch != '\t') ch = ' ';
    }
    double ty = content_top + (content_bottom - content_top - lines_max * line_h) / 2 + 14;
    size_t pos = 0;
    for (int l = 0; l < lines_max && pos < text.size(); ++l) {
      size_t nl = text.find('\n', pos);
      std::string line_str = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
      pos = (nl == std::string::npos) ? text.size() : nl + 1;
      if (line_str.size() > 48) line_str = line_str.substr(0, 45) + "...";
      cairo_text_extents_t te;
      cairo_text_extents(cr, line_str.c_str(), &te);
      double tx = px + (pw - te.width) / 2;
      cairo_move_to(cr, tx, ty);
      cairo_show_text(cr, line_str.c_str());
      ty += line_h;
    }
  } else if (app.preview_thumb) {
    int tw = cairo_image_surface_get_width(app.preview_thumb);
    int th = cairo_image_surface_get_height(app.preview_thumb);

    if (fill_preview && tw > 0 && th > 0) {
      int margin = 12;
      int bottom_h = 50;
      int img_x = px + margin;
      int img_y = py + margin;
      int img_w = pw - margin * 2;
      int img_h = (py + ph - bottom_h) - img_y - margin;
      double scale = std::min(static_cast<double>(img_w) / tw,
                              static_cast<double>(img_h) / th);
      int dw = static_cast<int>(tw * scale);
      int dh = static_cast<int>(th * scale);
      int dx = img_x + (img_w - dw) / 2;
      int dy = img_y + (img_h - dh) / 2;
      cairo_save(cr);
      cairo_rectangle(cr, dx, dy, dw, dh);
      cairo_clip(cr);
      cairo_set_source_surface(cr, app.preview_thumb, dx, dy);
      cairo_paint(cr);
      cairo_restore(cr);
    } else if (pdf && tw > 0 && th > 0) {
      int pdf_margin = 8;
      int page_w = pw - pdf_margin * 2;
      int page_h = content_bottom - content_top - pdf_margin;
      double scale = std::min(static_cast<double>(page_w) / tw,
                              static_cast<double>(page_h) / th);
      int dw = static_cast<int>(tw * scale);
      int dh = static_cast<int>(th * scale);
      int dx = px + (pw - dw) / 2;
      int dy = content_top + pdf_margin + (page_h - dh) / 2;
      cairo_save(cr);
      cairo_rectangle(cr, dx, dy, dw, dh);
      cairo_set_source_rgba(cr, 1, 1, 1, 1);
      cairo_fill(cr);
      cairo_rectangle(cr, dx, dy, dw, dh);
      cairo_clip(cr);
      cairo_translate(cr, dx, dy);
      cairo_scale(cr, scale, scale);
      cairo_set_source_surface(cr, app.preview_thumb, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    } else if (tw > 0 && th > 0) {
      if (space_mode) {
        int img_margin = 12;
        int avail_w = pw - img_margin * 2;
        int avail_h = ph - img_margin * 2 - 50;
        double s = std::min(static_cast<double>(avail_w) / tw,
                            static_cast<double>(avail_h) / th);
        int dw = static_cast<int>(tw * s);
        int dh = static_cast<int>(th * s);
        int dx = px + (pw - dw) / 2;
        int dy = py + (ph - 50 - dh) / 2;
        cairo_save(cr);
        cairo_rectangle(cr, dx, dy, dw, dh);
        cairo_clip(cr);
        cairo_translate(cr, dx, dy);
        cairo_scale(cr, s, s);
        cairo_set_source_surface(cr, app.preview_thumb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        int ts = std::min(pw - 20, content_bottom - content_top - 10);
        if (ts > 180) ts = 180;
        double scale = static_cast<double>(ts) / std::max(tw, th);
        int dw = static_cast<int>(tw * scale);
        int dh = static_cast<int>(th * scale);
        int dx = px + (pw - dw) / 2;
        int dy = content_top + (content_bottom - content_top - dh) / 2;
        cairo_save(cr);
        cairo_rectangle(cr, dx, dy, dw, dh);
        cairo_clip(cr);
        cairo_translate(cr, dx, dy);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, app.preview_thumb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
      }
    }
  }

  // File name (truncated if long, keeps extension)
  double color_adj = 0.9;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  if (name.size() > 30) {
    auto dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) {
      std::string ext = name.substr(dot);
      int keep = 27 - static_cast<int>(ext.size());
      if (keep > 0)
        name = name.substr(0, static_cast<size_t>(keep)) + "..." + ext;
      else
        name = name.substr(0, 27) + "...";
    } else {
      name = name.substr(0, 27) + "...";
    }
  }
  cairo_set_source_rgba(cr, app.text_r * color_adj, app.text_g * color_adj, app.text_b * color_adj, 0.9);
  cairo_text_extents_t te;
  cairo_text_extents(cr, name.c_str(), &te);
  double name_x = px + (pw - te.width) / 2;
  double name_y = py + ph - 36;
  cairo_move_to(cr, name_x, name_y);
  cairo_show_text(cr, name.c_str());

  // File info (size + type)
  cairo_set_font_size(cr, 11);
  cairo_set_source_rgba(cr, app.text_secondary_r * color_adj, app.text_secondary_g * color_adj, app.text_secondary_b * color_adj, 0.7);
  cairo_text_extents(cr, info.c_str(), &te);
  cairo_move_to(cr, px + (pw - te.width) / 2, name_y + 16);
  cairo_show_text(cr, info.c_str());
}

// ── search results banner ────────────────────────────────────────

void draw_search_banner(AppState& app, cairo_t* cr, int x, int y, int w) {
  constexpr int kBannerH = 28;
  double zf = app.zoom_pct / 100.0;

  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
  cairo_rectangle(cr, x, y, w, kBannerH);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.35);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, x, y + kBannerH - 0.5);
  cairo_line_to(cr, x + w, y + kBannerH - 0.5);
  cairo_stroke(cr);

  bool rec = app.active_pane ? app.r_recursive_search_active
                             : app.recursive_search_active;
  const auto& q = app.active_pane ? app.r_search_query : app.search_query;
  std::string root = rec ? home_dir() : app.cur_tab().current_path;
  if (root.size() > 48) root = "…" + root.substr(root.size() - 47);

  char count_buf[64];
  std::snprintf(count_buf, sizeof(count_buf), "%d",
                static_cast<int>(app.cur_tab().visible_entries.size()));

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12.0 * zf);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.85);
  std::string line = "Results for \u201C" + q + "\u201D in " + root +
                     " \u2014 " + count_buf + " items";
  cairo_move_to(cr, x + 12, y + kBannerH / 2 + 4);
  cairo_show_text(cr, line.c_str());

  // Clear button (right side)
  const char* clear_label = "Clear Search";
  cairo_text_extents_t te;
  cairo_text_extents(cr, clear_label, &te);
  int bw = static_cast<int>(te.width) + static_cast<int>(16.0 * zf);
  int bh = static_cast<int>(20.0 * zf);
  int bx = x + w - bw - 8;
  int by = y + (kBannerH - bh) / 2;
  app.search_banner_clear_x = bx;
  app.search_banner_clear_w = bw;

  bool hv = app.search_banner_clear_hover;
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b,
                        hv ? 0.9 : 0.6);
  draw_rounded_rect(cr, bx, by, bw, bh, 5);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  draw_rounded_rect(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, 5);
  cairo_stroke(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
  cairo_move_to(cr, bx + 8, by + bh / 2 + 4);
  cairo_show_text(cr, clear_label);
}

// ── sort menu dropdown ───────────────────────────────────────────

static const std::vector<SortMenuRow>& sort_menu_rows() {
  using K = SortMenuRow::Kind;
  static const std::vector<SortMenuRow> rows = {
    {K::Field, "Name", static_cast<int>(SortField::Name)},
    {K::Field, "Size", static_cast<int>(SortField::Size)},
    {K::Field, "Date Modified", static_cast<int>(SortField::Modified)},
    {K::Field, "Type", static_cast<int>(SortField::Type)},
    {K::Field, "Owner", static_cast<int>(SortField::Owner)},
    {K::Field, "Group", static_cast<int>(SortField::Group)},
    {K::Field, "Permissions", static_cast<int>(SortField::Permissions)},
    {K::Field, "Extension", static_cast<int>(SortField::Extension)},
    {K::Field, "Link Target", static_cast<int>(SortField::LinkTarget)},
    {K::Field, "First Modified", static_cast<int>(SortField::FirstModified)},
    {K::Field, "Last Modified", static_cast<int>(SortField::LastModified)},
    {K::Separator, "", 0},
    {K::ToggleDescending, "Descending", 0},
    {K::ToggleFoldersFirst, "Folders First", 0},
    {K::ToggleHiddenLast, "Hidden Last", 0},
    {K::ToggleNatural, "Natural Order", 0},
    {K::ToggleCaseSensitive, "Case Sensitive", 0},
    {K::Separator, "", 0},
    {K::GroupCaption, "Group By", 0},
    {K::GroupField, "No Grouping", 0},
    {K::GroupField, "Type", 1},
    {K::GroupField, "Name", 2},
    {K::GroupField, "Date", 3},
    {K::GroupField, "Size", 4},
  };
  return rows;
}

int sort_menu_row_count() { return static_cast<int>(sort_menu_rows().size()); }

const SortMenuRow& sort_menu_row(int index) {
  return sort_menu_rows()[static_cast<size_t>(index)];
}

void draw_sort_menu(AppState& app, cairo_t* cr) {
  // Per-pane position helpers
  auto& dm_sort_menu_x = app.active_pane ? app.r_sort_menu_x : app.sort_menu_x;
  auto& dm_sort_menu_y = app.active_pane ? app.r_sort_menu_y : app.sort_menu_y;
  auto& dm_sort_menu_w = app.active_pane ? app.r_sort_menu_w : app.sort_menu_w;
  auto& dm_sort_menu_h = app.active_pane ? app.r_sort_menu_h : app.sort_menu_h;
  auto& dm_sort_menu_hover = app.active_pane ? app.r_sort_menu_hover : app.sort_menu_hover;
  auto& dm_sort_btn_x = app.active_pane ? app.r_sort_btn_x : app.sort_btn_x;

  static constexpr int kItemH = kSortMenuItemH;
  static constexpr int kPad = kSortMenuPad;
  int n = sort_menu_row_count();
  int menu_w = 180;
  int menu_h = n * kItemH + kPad * 2;

  // Position below the sort button
  int top_h = app.top_bar_height;
  dm_sort_menu_x = dm_sort_btn_x;
  dm_sort_menu_y = top_h;
  dm_sort_menu_w = menu_w;
  dm_sort_menu_h = menu_h;

  // Shadow
  for (int s = 3; s >= 0; --s) {
    double a = 0.08 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, dm_sort_menu_x + s * 2, dm_sort_menu_y + s * 2, menu_w, menu_h, 6);
    cairo_fill(cr);
  }

  // Background
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 1.0);
  draw_rounded_rect(cr, dm_sort_menu_x, dm_sort_menu_y, menu_w, menu_h, 6);
  cairo_fill(cr);

  // Outline
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, dm_sort_menu_x + 0.5, dm_sort_menu_y + 0.5, menu_w - 1, menu_h - 1, 5.5);
  cairo_stroke(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);

  for (int i = 0; i < n; ++i) {
    int row_y = dm_sort_menu_y + kPad + i * kItemH;
    const SortMenuRow& row = sort_menu_row(i);
    bool hovered = (i == dm_sort_menu_hover);

    if (row.kind == SortMenuRow::Kind::Separator) {
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
      cairo_set_line_width(cr, 1);
      cairo_move_to(cr, dm_sort_menu_x + 10, row_y + kItemH / 2 + 0.5);
      cairo_line_to(cr, dm_sort_menu_x + menu_w - 10, row_y + kItemH / 2 + 0.5);
      cairo_stroke(cr);
      continue;
    }

    bool active = false;
    switch (row.kind) {
      case SortMenuRow::Kind::Field:
        active = static_cast<int>(app.cur_tab().sort_field) == row.field;
        break;
      case SortMenuRow::Kind::ToggleDescending:
        active = app.cur_tab().sort_descending;
        break;
      case SortMenuRow::Kind::ToggleFoldersFirst:
        active = app.folders_before_files;
        break;
      case SortMenuRow::Kind::ToggleHiddenLast:
        active = app.sort_hidden_last;
        break;
      case SortMenuRow::Kind::ToggleNatural:
        active = app.sort_natural;
        break;
      case SortMenuRow::Kind::ToggleCaseSensitive:
        active = app.sort_case_sensitive;
        break;
      case SortMenuRow::Kind::GroupField:
        active = app.cur_tab().group_field == row.field;
        break;
      default:
        break;
    }

    if (row.kind == SortMenuRow::Kind::GroupCaption) {
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                             app.text_secondary_b, 0.8);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 11);
      cairo_move_to(cr, dm_sort_menu_x + 14, row_y + kItemH / 2 + 4);
      cairo_show_text(cr, row.label);
      cairo_set_font_size(cr, 13);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_NORMAL);
      continue;
    }

    if (hovered) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      draw_rounded_rect(cr, dm_sort_menu_x + 4, row_y, menu_w - 8, kItemH, 4);
      cairo_fill(cr);
    }

    // Checkmark for current sort field / enabled toggle
    if (active) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
      cairo_move_to(cr, dm_sort_menu_x + 14, row_y + kItemH / 2 + 4);
      cairo_show_text(cr, "✓ ");
    }

    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, dm_sort_menu_x + 14 + (active ? 14 : 0), row_y + kItemH / 2 + 4);
    cairo_show_text(cr, row.label);

    // Direction arrow beside the active field
    if (row.kind == SortMenuRow::Kind::Field && active) {
      cairo_move_to(cr, dm_sort_menu_x + menu_w - 20, row_y + kItemH / 2 + 4);
      cairo_show_text(cr, app.cur_tab().sort_descending ? "↑" : "↓");
    }
  }
}

// ── column chooser popup ─────────────────────────────────────────

void draw_columns_menu(AppState& app, cairo_t* cr) {
  auto& cm_x = app.active_pane ? app.r_columns_menu_x : app.columns_menu_x;
  auto& cm_y = app.active_pane ? app.r_columns_menu_y : app.columns_menu_y;
  auto& cm_w = app.active_pane ? app.r_columns_menu_w : app.columns_menu_w;
  auto& cm_h = app.active_pane ? app.r_columns_menu_h : app.columns_menu_h;
  auto& cm_hover = app.active_pane ? app.r_columns_menu_hover : app.columns_menu_hover;

  struct ColRow { const char* label; bool* val; };
  ColRow rows[] = {
    {"Owner", &app.col_owner},
    {"Group", &app.col_group},
    {"Permissions", &app.col_perms},
    {"Extension", &app.col_ext},
    {"Link Target", &app.col_target},
  };
  constexpr int kRows = 5;
  int menu_w = 170;
  int menu_h = kRows * kSortMenuItemH + kSortMenuPad * 2;
  cm_w = menu_w;
  cm_h = menu_h;

  for (int s = 3; s >= 0; --s) {
    double a = 0.08 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, cm_x + s * 2, cm_y + s * 2, menu_w, menu_h, 6);
    cairo_fill(cr);
  }
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 1.0);
  draw_rounded_rect(cr, cm_x, cm_y, menu_w, menu_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, cm_x + 0.5, cm_y + 0.5, menu_w - 1, menu_h - 1, 5.5);
  cairo_stroke(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);

  for (int i = 0; i < kRows; ++i) {
    int row_y = cm_y + kSortMenuPad + i * kSortMenuItemH;
    if (i == cm_hover) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      draw_rounded_rect(cr, cm_x + 4, row_y, menu_w - 8, kSortMenuItemH, 4);
      cairo_fill(cr);
    }
    if (*rows[i].val) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
      cairo_move_to(cr, cm_x + 14, row_y + kSortMenuItemH / 2 + 4);
      cairo_show_text(cr, "✓ ");
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, cm_x + 14 + (*rows[i].val ? 14 : 0),
                  row_y + kSortMenuItemH / 2 + 4);
    cairo_show_text(cr, rows[i].label);
  }
}

// ── list view ────────────────────────────────────────────────────

// Shared Group By header band (list/grid/compact). pinned=true adds a
// shadow so the sticky header reads as floating above content.
static void draw_group_header_band(AppState& app, cairo_t* cr, int x, int y,
                                    int w, int h, const std::string& label,
                                    bool pinned = false) {
  double zf = app.zoom_pct / 100.0;
  if (pinned) {
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.18);
    cairo_rectangle(cr, x, y + h, w, 3);
    cairo_fill(cr);
  }
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b,
                        pinned ? 1.0 : 1.0);
  cairo_rectangle(cr, x, y, w, h);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.06);
  cairo_rectangle(cr, x, y, w, h);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.5);
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 11.0 * zf);
  cairo_move_to(cr, x + static_cast<int>(44.0 * zf), y + h - 6);
  cairo_show_text(cr, label.c_str());
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.12);
  cairo_move_to(cr, x, y + h - 1);
  cairo_line_to(cr, x + w, y + h - 1);
  cairo_stroke(cr);
}

static void draw_column_header(AppState& app, cairo_t* cr, int x, int y, int w, int h,
                                const char* label,
                                bool divider_hover) {
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
  cairo_move_to(cr, x + 6, y + h / 2 + 4);
  cairo_show_text(cr, label);

  // Divider line
  if (divider_hover) {
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.6);
  } else {
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  }
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, x + w, y);
  cairo_line_to(cr, x + w, y + h);
  cairo_stroke(cr);
}

// ── draw_tab_bar ─────────────────────────────────────────────────

void draw_tab_bar(AppState& app, cairo_t* cr, int w, int tab_h, int pane_x, int pane_w) {
  double zf = app.zoom_pct / 100.0;

  int tab_count = static_cast<int>(app.tabs.size());
  app.tab_hits.resize(tab_count);

  int sidebar_w;
  if (pane_w > 0) {
    sidebar_w = pane_x;
  } else {
    sidebar_w = app.sidebar_expanded ? app.sidebar_width : 0;
  }

  int x = sidebar_w;
  int close_icon_sz = static_cast<int>(7.0 * zf);
  int pad = static_cast<int>(12.0 * zf);
  int font_size = static_cast<int>(14.0 * zf);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, font_size);

  bool dragging = app.tab_dragging;
  int drag_sb_idx = dragging ? app.tab_drag_from : -1;

  for (int i = 0; i < tab_count; ++i) {
    // Compute label from path basename
    std::string label = app.tabs[i].current_path;
    auto pos = label.rfind('/');
    if (pos != std::string::npos) label = label.substr(pos + 1);
    if (label.empty()) label = "/";

    // Measure text width
    cairo_text_extents_t te;
    cairo_text_extents(cr, label.c_str(), &te);

    int close_w = close_icon_sz + pad;
    int label_w = static_cast<int>(te.x_advance);
    int tab_w = pad + label_w + pad + close_w + pad;
    int min_tab_w = static_cast<int>(100.0 * zf);
    if (tab_w < min_tab_w) tab_w = min_tab_w;

    // Clamp to available width
    int avail = w - x;
    if (tab_w > avail) tab_w = avail;

    bool active = (i == app.active_tab);

    // Active tab glass effect
    if (active) {
      int r = static_cast<int>(12.0 * zf);
      int m = static_cast<int>(1.0 * zf);
      int l = x + m;
      int t = m;
      int rw = tab_w - m * 2;
      int rh = tab_h - 1 - m * 2;
      cairo_new_path(cr);
      cairo_arc(cr, l + r, t + r, r, M_PI, 1.5 * M_PI);
      cairo_arc(cr, l + rw - r, t + r, r, 1.5 * M_PI, 2.0 * M_PI);
      cairo_arc(cr, l + rw - r, t + rh - r, r, 0.0, 0.5 * M_PI);
      cairo_arc(cr, l + r, t + rh - r, r, 0.5 * M_PI, M_PI);
      cairo_close_path(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.15);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.4);
      cairo_set_line_width(cr, 1.5);
      cairo_stroke(cr);
    }

    // Drop target glow on tab header (during file drag)
    if (i == app.drop_target_tab_idx && !active) {
      double pulse = 0.14 + 0.06 * std::sin(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count() * 0.006);
      int r = static_cast<int>(12.0 * zf);
      int m = static_cast<int>(1.0 * zf);
      int l = x + m;
      int t = m;
      int rw = tab_w - m * 2;
      int rh = tab_h - 1 - m * 2;
      cairo_new_path(cr);
      cairo_arc(cr, l + r, t + r, r, M_PI, 1.5 * M_PI);
      cairo_arc(cr, l + rw - r, t + r, r, 1.5 * M_PI, 2.0 * M_PI);
      cairo_arc(cr, l + rw - r, t + rh - r, r, 0.0, 0.5 * M_PI);
      cairo_arc(cr, l + r, t + rh - r, r, 0.5 * M_PI, M_PI);
      cairo_close_path(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, pulse);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, pulse + 0.15);
      cairo_set_line_width(cr, 1.5);
      cairo_stroke(cr);
      app.pendingRedraw = true;
    }

    // Label (dimmed if being dragged)
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            active ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    if (dragging && i == drag_sb_idx) {
      cairo_push_group(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, active ? 1.0 : 0.7);
    cairo_move_to(cr, x + pad, tab_h / 2 + static_cast<int>(5.0 * zf));
    cairo_show_text(cr, label.c_str());

    // Close button
    int close_x = x + tab_w - pad - close_icon_sz;
    int close_y = (tab_h - close_icon_sz) / 2;
    // Draw close "×"
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                          app.dots_btn_hover ? 0.85 : 0.5);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, close_x, close_y);
    cairo_line_to(cr, close_x + close_icon_sz, close_y + close_icon_sz);
    cairo_move_to(cr, close_x + close_icon_sz, close_y);
    cairo_line_to(cr, close_x, close_y + close_icon_sz);
    cairo_stroke(cr);

    if (dragging && i == drag_sb_idx) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.35);
    }

    // Store hit rect
    app.tab_hits[i].x = x;
    app.tab_hits[i].w = tab_w;
    app.tab_hits[i].close_x = close_x;

    x += tab_w;
  }

  // Dragged tab insertion line
  if (dragging) {
    int slot = app.tab_drag_to_visual;
    int line_x;
    if (slot == 0) {
      line_x = app.tab_hits[0].x;
    } else if (slot >= tab_count) {
      line_x = app.tab_hits[tab_count - 1].x + app.tab_hits[tab_count - 1].w;
    } else {
      line_x = app.tab_hits[slot].x;
    }
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, line_x, static_cast<int>(4.0 * zf));
    cairo_line_to(cr, line_x, tab_h - static_cast<int>(4.0 * zf));
    cairo_stroke(cr);
  }

  // Ghost tab following cursor
  if (dragging && drag_sb_idx >= 0 && drag_sb_idx < tab_count) {
    int ghost_x = app.tab_drag_current_x - app.tab_hits[drag_sb_idx].w / 2;
    int ghost_w = app.tab_hits[drag_sb_idx].w;
    int r = static_cast<int>(12.0 * zf);
    int m = static_cast<int>(1.0 * zf);
    cairo_new_path(cr);
    cairo_arc(cr, ghost_x + m + r, m + r, r, M_PI, 1.5 * M_PI);
    cairo_arc(cr, ghost_x + ghost_w - m - r, m + r, r, 1.5 * M_PI, 2.0 * M_PI);
    cairo_arc(cr, ghost_x + ghost_w - m - r, tab_h - 1 - m - r, r, 0.0, 0.5 * M_PI);
    cairo_arc(cr, ghost_x + m + r, tab_h - 1 - m - r, r, 0.5 * M_PI, M_PI);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.5);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
  }
}

void draw_list_view(AppState& app, cairo_t* cr, int content_x,
                    int content_y, int content_w, int view_h) {
  double zf = app.zoom_pct / 100.0;
  int entry_h = app.entry_height;
  int text_x = content_x + static_cast<int>(40.0 * zf);
  int icon_size = static_cast<int>(24.0 * zf);
  auto col_w = [&](bool on, int base) {
    return on ? static_cast<int>(base * zf) : 0;
  };
  int own_w  = col_w(app.col_owner, 90);
  int grp_w  = col_w(app.col_group, 90);
  int prm_w  = col_w(app.col_perms, 84);
  int ext_w  = col_w(app.col_ext, 70);
  int tgt_w  = col_w(app.col_target, 150);
  int extra_total = own_w + grp_w + prm_w + ext_w + tgt_w;
  int name_w = std::max(static_cast<int>(120 * zf),
                        static_cast<int>(content_w * app.col_name_frac) - extra_total);
  int size_w = static_cast<int>(content_w * app.col_size_frac);
  int date_w = static_cast<int>(content_w * app.col_date_frac);
  int name_x = text_x;
  int size_x = name_x + name_w;
  int date_x = size_x + size_w;
  int own_x  = date_x + date_w;
  int grp_x  = own_x + own_w;
  int prm_x  = grp_x + grp_w;
  int ext_x  = prm_x + prm_w;
  int tgt_x  = ext_x + ext_w;
  int type_x = tgt_x + tgt_w;
  int type_w = content_w - (type_x - content_x);

  // ── Column header row ──
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.05);
  cairo_rectangle(cr, content_x, content_y, content_w, entry_h);
  cairo_fill(cr);
  draw_column_header(app, cr, name_x, content_y, name_w, entry_h, "Name",
                      app.col_resizing == 0 || app.col_hover_divider);
  draw_column_header(app, cr, size_x, content_y, size_w, entry_h, "Size",
                      app.col_resizing == 1 || app.col_hover_divider);
  draw_column_header(app, cr, date_x, content_y, date_w, entry_h, "Date",
                      app.col_resizing == 2 || app.col_hover_divider);
  if (app.col_show_type) {
    draw_column_header(app, cr, type_x, content_y, type_w, entry_h, "Type",
                        app.col_resizing == 3 || app.col_hover_divider);
  }
  if (app.col_owner)
    draw_column_header(app, cr, own_x, content_y, own_w, entry_h, "Owner", false);
  if (app.col_group)
    draw_column_header(app, cr, grp_x, content_y, grp_w, entry_h, "Group", false);
  if (app.col_perms)
    draw_column_header(app, cr, prm_x, content_y, prm_w, entry_h, "Perms", false);
  if (app.col_ext)
    draw_column_header(app, cr, ext_x, content_y, ext_w, entry_h, "Ext", false);
  if (app.col_target)
    draw_column_header(app, cr, tgt_x, content_y, tgt_w, entry_h, "Link Target", false);

  int y = content_y + entry_h - app.cur_tab().scroll_px;

  std::string prev_group;
  int header_h = static_cast<int>(entry_h * 0.55);
  std::unordered_map<std::string, int> header_ys;
  bool sticky_recorded = false;
  std::string first_vis_label;
  int first_vis_header_y = INT_MIN;

  for (int vi = 0; vi < static_cast<int>(app.cur_tab().visible_entries.size()); ++vi) {
    int real_idx = app.cur_tab().visible_entries[vi];
    if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
      continue;
    auto& entry = app.cur_tab().entries[real_idx];

    std::string row_label;
    if (app.cur_tab().group_field > 0) {
      row_label = group_label_for(app, entry);
      if (row_label != prev_group) {
        prev_group = row_label;
        header_ys[row_label] = y;
        if (y + header_h >= content_y)
          draw_group_header_band(app, cr, content_x, y, content_w, header_h, row_label);
        y += header_h;
      }
      if (!sticky_recorded && y + entry_h >= content_y) {
        sticky_recorded = true;
        first_vis_label = row_label;
        auto it = header_ys.find(row_label);
        first_vis_header_y = (it != header_ys.end()) ? it->second : INT_MIN;
      }
    }

    if (y + entry_h < content_y) { y += entry_h; continue; }
    if (y > content_y + view_h) break;

    bool selected =
        vi == app.cur_tab().selected_idx ||
        std::find(app.cur_tab().multi_selected.begin(), app.cur_tab().multi_selected.end(), vi) !=
            app.cur_tab().multi_selected.end();
    bool hovered = vi == app.cur_tab().hover_idx;
    bool drop_target = !app.drop_target_path.empty() && !app.drop_target_is_sidebar &&
                       vi == app.drop_target_idx && entry.is_dir;
    bool is_cut = !app.cut_paths.empty() && app.cut_paths.count(entry.path);

    if (selected) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                             0.25);
      cairo_rectangle(cr, content_x, y, content_w, entry_h);
      cairo_fill(cr);
    } else if (drop_target) {
      double pulse = 0.18 + 0.07 * std::sin(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count() * 0.006);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, pulse);
      cairo_rectangle(cr, content_x, y, content_w, entry_h);
      cairo_fill(cr);
      app.pendingRedraw = true;
    } else if (hovered) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.06);
      cairo_rectangle(cr, content_x, y, content_w, entry_h);
      cairo_fill(cr);
    }

    // Cut indicator: dashed border on cut files (GNOME 49 style)
    if (is_cut) {
      cairo_save(cr);
      double dash_len = 5.0;
      double gap_len = 3.0;
      cairo_set_dash(cr, &dash_len, 1, 0.0);
      cairo_set_line_width(cr, 1.5);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                             app.text_secondary_b, 0.6);
      draw_rounded_rect(cr, content_x + 1, y + 1, content_w - 2, entry_h - 2,
                        static_cast<int>(4.0 * zf));
      cairo_stroke(cr);
      cairo_set_dash(cr, &dash_len, 0, 0.0);
      cairo_restore(cr);
    }

    bool hidden = entry.is_hidden;
    if (hidden || is_cut) cairo_push_group(cr);

    cairo_surface_t* thumb = nullptr;
    if (entry.type == FileType::Image || entry.type == FileType::Video) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    } else if (entry.type == FileType::Document && (is_pdf_extension(entry.path) || is_epub_extension(entry.path))) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    }
    draw_file_icon_cairo(app, cr, content_x + 8, y + (entry_h - icon_size) / 2,
                          icon_size, entry.type, selected, entry.icon_name, thumb, &entry.path, &entry);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0 * zf);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, text_x, y + entry_h / 2 + 4);

    std::string display_name = entry.name;
    cairo_text_extents_t te;
    cairo_text_extents(cr, display_name.c_str(), &te);
    if (te.width > name_w - 20) {
      std::string ext;
      auto dot = display_name.rfind('.');
      if (dot != std::string::npos && dot > 0) {
        ext = display_name.substr(dot);
        display_name = display_name.substr(0, dot);
      }
      if (ext.empty()) {
        while (!display_name.empty() && te.width > name_w - 24) {
          display_name.pop_back();
          cairo_text_extents(cr, (display_name + "...").c_str(), &te);
        }
        display_name += "...";
      } else {
        while (!display_name.empty()) {
          cairo_text_extents(cr, (display_name + "..." + ext).c_str(), &te);
          if (te.width <= name_w - 24) break;
          display_name.pop_back();
        }
        display_name += "..." + ext;
      }
    }
    cairo_show_text(cr, display_name.c_str());

    if (!entry.is_dir) {
      cairo_set_font_size(cr, 12.0 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, 1.0);
      cairo_move_to(cr, size_x, y + entry_h / 2 + 4);
      cairo_show_text(cr, format_size(entry.size).c_str());
    }

    cairo_set_font_size(cr, 12.0 * zf);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 1.0);
    cairo_move_to(cr, date_x, y + entry_h / 2 + 4);
    struct tm tm_buf;
    struct tm* lt = localtime_r(&entry.modified_sec, &tm_buf);
    if (lt) {
      char date_buf[32];
      strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", lt);
      cairo_show_text(cr, date_buf);
    }

    // Optional stat-based columns
    if (extra_total > 0) {
      cairo_set_font_size(cr, 12.0 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                             app.text_secondary_b, 1.0);
      auto clip_show = [&](int cx0, int cw0, const std::string& s) {
        if (cw0 <= 0 || s.empty()) return;
        cairo_save(cr);
        cairo_rectangle(cr, cx0, y, cw0 - 6, entry_h);
        cairo_clip(cr);
        cairo_move_to(cr, cx0, y + entry_h / 2 + 4);
        cairo_show_text(cr, s.c_str());
        cairo_restore(cr);
      };
      clip_show(own_x, own_w, entry.owner);
      clip_show(grp_x, grp_w, entry.group);
      clip_show(prm_x, prm_w,
                entry.mode ? format_mode(entry.mode) : std::string());
      clip_show(ext_x, ext_w, entry.extension);
      clip_show(tgt_x, tgt_w,
                entry.link_target.empty() && !entry.is_dir
                    ? std::string()
                    : entry.link_target);
    }

    if (hidden) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.5);
    } else if (is_cut) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.5);
    }

    y += entry_h;
  }

  // Sticky group header pinned to the top of the viewport
  if (app.cur_tab().group_field > 0 && sticky_recorded &&
      first_vis_header_y != INT_MIN && first_vis_header_y < content_y)
    draw_group_header_band(app, cr, content_x, content_y, content_w, header_h,
                            first_vis_label, true);

  app.cur_tab().content_h = y - content_y + app.cur_tab().scroll_px - entry_h;
}



// ── grid view ────────────────────────────────────────────────────

void draw_grid_view(AppState& app, cairo_t* cr, int content_x,
                    int content_y, int content_w, int view_h) {
  double zf = app.zoom_pct / 100.0;

  // Grid: auto-fill, minmax(110px, 1fr). Column gap and row gap are
  // intentionally different — Nautilus/Dolphin use a noticeably tighter
  // vertical rhythm than horizontal, so reusing one constant for both
  // (as before) made rows much taller/looser than the reference UIs.
  int min_cell_w = static_cast<int>(110.0 * zf);
  int col_gap = static_cast<int>(18.0 * zf);
  int row_gap = static_cast<int>(10.0 * zf);
  int cols = std::max(1, (content_w + col_gap) / (min_cell_w + col_gap));
  int cell_w = (content_w - col_gap - (cols - 1) * col_gap) / cols;
  app.grid_cell_size = cell_w;
  app.grid_cols = cols;
  app.grid_cell_gap = col_gap;

  // Icon area, capped smaller than before — large 112px icons at typical
  // cell widths read oversized next to Nautilus's ~64-72px default.
  int icon_size = std::min(cell_w - static_cast<int>(16.0 * zf),
                           static_cast<int>(72.0 * zf));
  int icon_area = icon_size;
  int label_h = static_cast<int>(32.0 * zf); // 2 lines of label text
  int text_gap = static_cast<int>(4.0 * zf); // tight, label sits close under icon
  int item_h = icon_area + text_gap + label_h;
  int row_h = item_h + row_gap;
  app.grid_row_h = row_h;

  int grid_w = cols * cell_w + (cols - 1) * col_gap;
  int grid_offset_x = (content_w - grid_w) / 2;

  int y = content_y + row_gap - app.cur_tab().scroll_px;

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * zf);

  int group_extra = 0;
  std::string prev_group;
  int gheader_h = static_cast<int>(22.0 * zf);
  std::unordered_map<std::string, int> gheader_ys;
  bool sticky_recorded = false;
  std::string first_vis_label;
  int first_vis_header_y = INT_MIN;

  for (int vi = 0; vi < static_cast<int>(app.cur_tab().visible_entries.size()); ++vi) {
    int col = vi % cols;
    int row = vi / cols;

    // Group By: new band at each row where the group changes
    if (app.cur_tab().group_field > 0 && col == 0) {
      int gri = app.cur_tab().visible_entries[vi];
      if (gri >= 0 && gri < static_cast<int>(app.cur_tab().entries.size())) {
        std::string label = group_label_for(app, app.cur_tab().entries[gri]);
        if (label != prev_group) {
          prev_group = label;
          int hy = y + row * row_h + group_extra;
          gheader_ys[label] = hy;
          if (hy + gheader_h >= content_y)
            draw_group_header_band(app, cr, content_x, hy, content_w,
                                    gheader_h, label);
          group_extra += gheader_h;
        }
        if (!sticky_recorded &&
            y + row * row_h + group_extra + item_h >= content_y) {
          sticky_recorded = true;
          first_vis_label = label;
          auto it = gheader_ys.find(label);
          first_vis_header_y = (it != gheader_ys.end()) ? it->second : INT_MIN;
        }
      }
    }

    int cx = content_x + grid_offset_x + col * (cell_w + col_gap);
    int cy = y + row * row_h + group_extra;

    if (cy + item_h < content_y) continue;
    if (cy > content_y + view_h) break;

    int real_idx = app.cur_tab().visible_entries[vi];
    if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
      continue;
    auto& entry = app.cur_tab().entries[real_idx];

    bool selected =
        vi == app.cur_tab().selected_idx ||
        std::find(app.cur_tab().multi_selected.begin(), app.cur_tab().multi_selected.end(), vi) !=
            app.cur_tab().multi_selected.end();
    bool hovered = vi == app.cur_tab().hover_idx;
    bool drop_target = !app.drop_target_path.empty() && !app.drop_target_is_sidebar &&
                       vi == app.drop_target_idx && entry.is_dir;
    bool is_cut = !app.cut_paths.empty() && app.cut_paths.count(entry.path);

    bool hidden = entry.is_hidden;
    if (hidden || is_cut) cairo_push_group(cr);

    int bg_x = cx + (cell_w - icon_size) / 2;
    int bg_y = cy;

    // Selection/hover outline
    if (selected) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.35);
      cairo_set_line_width(cr, 2.0);
      draw_rounded_rect(cr, bg_x + 1, bg_y + 1, icon_size - 2, icon_size - 2,
                        static_cast<int>(8.0 * zf));
      cairo_stroke(cr);
    } else if (drop_target) {
      double pulse = 0.30 + 0.10 * std::sin(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count() * 0.006);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, pulse);
      cairo_set_line_width(cr, 2.0);
      draw_rounded_rect(cr, bg_x + 1, bg_y + 1, icon_size - 2, icon_size - 2,
                        static_cast<int>(8.0 * zf));
      cairo_stroke(cr);
      app.pendingRedraw = true;
    } else if (hovered) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.06);
      draw_rounded_rect(cr, bg_x, bg_y, icon_size, icon_size,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
    }

    // Cut indicator: dashed border on cut files (GNOME 49 style)
    if (is_cut) {
      cairo_save(cr);
      double dash_len = 5.0;
      double gap_len = 3.0;
      cairo_set_dash(cr, &dash_len, 1, 0.0);
      cairo_set_line_width(cr, 1.5);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                             app.text_secondary_b, 0.6);
      draw_rounded_rect(cr, bg_x + 1, bg_y + 1, icon_size - 2, icon_size - 2,
                        static_cast<int>(8.0 * zf));
      cairo_stroke(cr);
      cairo_set_dash(cr, &dash_len, 0, 0.0);
      cairo_restore(cr);
    }

    // File icon
    cairo_surface_t* thumb = nullptr;
    if (entry.type == FileType::Image || entry.type == FileType::Video) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    } else if (entry.type == FileType::Document && (is_pdf_extension(entry.path) || is_epub_extension(entry.path))) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    }
    draw_file_icon_cairo(app, cr, bg_x, bg_y,
                          icon_size, entry.type, selected, entry.icon_name, thumb, &entry.path, &entry);

    // Label (word/char-wrapped to 2 lines; extension preserved if truncation
    // is needed; top-anchored so spacing is consistent regardless of 1 vs 2 lines)
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    auto* pl = pango_cairo_create_layout(cr);
    auto* desc = pango_font_description_new();
    pango_font_description_set_family(desc, "Sans");
    pango_font_description_set_absolute_size(desc, static_cast<int>(13.0 * zf * PANGO_SCALE));
    pango_layout_set_font_description(pl, desc);
    int layout_w = cell_w - 8;
    pango_layout_set_width(pl, static_cast<int>(layout_w * PANGO_SCALE));
    pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_NONE);

    // Try the full name first, with no height cap, so get_line_count()
    // reflects the true number of lines it would take.
    std::string label = entry.name;
    pango_layout_set_text(pl, label.c_str(), -1);

    if (pango_layout_get_line_count(pl) > 2) {
      // Doesn't fit in 2 lines as-is. Shrink the stem (not the extension)
      // a character at a time and re-test, same approach as before but
      // budgeting for 2 wrapped lines instead of 1.
      std::string ext;
      auto dot = label.rfind('.');
      if (dot != std::string::npos && dot > 0) {
        ext = label.substr(dot);
        label = label.substr(0, dot);
      }
      while (!label.empty()) {
        std::string candidate = ext.empty() ? (label + "...") : (label + "..." + ext);
        pango_layout_set_text(pl, candidate.c_str(), -1);
        if (pango_layout_get_line_count(pl) <= 2) break;
        label.pop_back();
      }
      if (label.empty()) {
        pango_layout_set_text(pl, (ext.empty() ? "..." : ("..." + ext)).c_str(), -1);
      }
    }

    int label_area_h = label_h;
    int line_h = static_cast<int>(16.0 * zf);
    int label_x = cx + (cell_w - layout_w) / 2;
    int label_y = cy + icon_size + text_gap + std::max(0, (label_area_h - 2 * line_h) / 2);
    cairo_move_to(cr, label_x, label_y);
    pango_cairo_show_layout(cr, pl);
    pango_font_description_free(desc);
    g_object_unref(pl);

    if (hidden || is_cut) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.5);
    }
  }

  int rows = (static_cast<int>(app.cur_tab().visible_entries.size()) + cols - 1) / cols;
  app.cur_tab().content_h = y + rows * row_h + group_extra - content_y + app.cur_tab().scroll_px + row_gap;

  // Sticky group header pinned to the top of the viewport
  if (app.cur_tab().group_field > 0 && sticky_recorded &&
      first_vis_header_y != INT_MIN && first_vis_header_y < content_y)
    draw_group_header_band(app, cr, content_x, content_y, content_w,
                            gheader_h, first_vis_label, true);
}

// ── status bar ───────────────────────────────────────────────────

void draw_status_bar(AppState& app, cairo_t* cr, int w, int h,
                     int status_h) {
  // Check operation status expiry
  if (!app.operation_status.empty() && app.operation_status_expires_ms > 0) {
    auto now = std::chrono::steady_clock::now();
    auto expiry = std::chrono::steady_clock::time_point(
        std::chrono::milliseconds(app.operation_status_expires_ms));
    if (now >= expiry) {
      app.operation_status.clear();
      app.operation_status_expires_ms = 0;
    }
  }

  double zf = app.zoom_pct / 100.0;
  int y = h - status_h;
  double sa = app.statusbar_opacity_pct / 100.0;

  // bg-zinc-900 style background
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, sa);
  cairo_rectangle(cr, 0, y, w, status_h);
  cairo_fill(cr);

  // border-t zinc-700
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  cairo_rectangle(cr, 0, y, w, 1);
  cairo_fill(cr);

  // px-6 = 24px padding
  int pad = static_cast<int>(24.0 * zf);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * zf); // text-sm
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);

  char status_buf[256];
  size_t sel_count = app.cur_tab().multi_selected.size();

  if (sel_count == 1) {
    int sel = app.cur_tab().multi_selected[0];
    int real_idx = (sel >= 0 && sel < static_cast<int>(app.cur_tab().visible_entries.size()))
                       ? app.cur_tab().visible_entries[sel]
                       : -1;
    if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size())) {
      auto& entry = app.cur_tab().entries[real_idx];
      if (entry.is_dir) {
        // Never walk trees on the paint thread: consult the background
        // dir-stats cache and request a refresh when stale/missing.
        struct ::stat dst{};
        int64_t dmtime = (::stat(entry.path.c_str(), &dst) == 0)
                             ? static_cast<int64_t>(dst.st_mtime) : 0;
        eh::file_browser::dir_stats_request(app, entry.path, dmtime);
        auto ds = app.dir_stat_cache.find(entry.path);
        if (ds != app.dir_stat_cache.end() && ds->second.mtime_sec == dmtime &&
            !ds->second.truncated) {
          std::snprintf(status_buf, sizeof(status_buf),
                        "%s/ \u2014 %llu items (%s)", entry.name.c_str(),
                        (unsigned long long)ds->second.count,
                        format_size(ds->second.bytes).c_str());
        } else if (ds != app.dir_stat_cache.end() &&
                   ds->second.mtime_sec == dmtime && ds->second.truncated) {
          std::snprintf(status_buf, sizeof(status_buf),
                        "%s/ \u2014 %llu+ items", entry.name.c_str(),
                        (unsigned long long)ds->second.count);
        } else {
          std::snprintf(status_buf, sizeof(status_buf), "%s/",
                        entry.name.c_str());
        }
      } else {
        std::snprintf(status_buf, sizeof(status_buf), "%s (%s)",
                      entry.name.c_str(), format_size(entry.size).c_str());
      }
    }
  } else if (sel_count > 1) {
    uint64_t total_size = 0;
    for (int sel : app.cur_tab().multi_selected) {
      int real_idx =
          (sel >= 0 && sel < static_cast<int>(app.cur_tab().visible_entries.size()))
              ? app.cur_tab().visible_entries[sel]
              : -1;
      if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()))
        total_size += app.cur_tab().entries[real_idx].size;
    }
    std::snprintf(status_buf, sizeof(status_buf), "%zu items selected (%s)",
                  sel_count, format_size(total_size).c_str());
  }

  if (sel_count == 0) {
    if ((app.search_active || app.recursive_search_active || app.r_search_active || app.r_recursive_search_active) && (!app.search_query.empty() || !app.r_search_query.empty())) {
      std::snprintf(status_buf, sizeof(status_buf), "%zu results",
                    app.cur_tab().entries.size());
    } else {
      // Aggregates are cached on the tab (rebuilt when entries change);
      // scanning 900k entries here every frame used to cost ~25 ms/frame.
      std::snprintf(status_buf, sizeof(status_buf),
                    "%d items (%d files, %d dirs)", app.cur_tab().cached_total_items,
                    app.cur_tab().cached_total_files, app.cur_tab().cached_total_dirs);
    }
  }

  if (!app.operation_status.empty()) {
    // Show operation status centered, overriding selection info
    cairo_text_extents_t te;
    cairo_text_extents(cr, app.operation_status.c_str(), &te);
    double sw = te.width;
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
    cairo_move_to(cr, (w - sw) / 2, y + status_h / 2 + 4);
    cairo_show_text(cr, app.operation_status.c_str());
  }

  cairo_move_to(cr, pad, y + status_h / 2 + 4);
  cairo_show_text(cr, status_buf);

  std::string free_str;
  {
    // statvfs can block for hundreds of ms on slow/network mounts — never
    // call it more than once per 2 s per path.
    static std::string s_path;
    static uint64_t s_free = 0;
    static bool s_valid = false;
    static std::chrono::steady_clock::time_point s_at{};
    auto now = std::chrono::steady_clock::now();
    if (!s_valid || s_path != app.cur_tab().current_path ||
        now - s_at > std::chrono::milliseconds(2000)) {
      struct statvfs sv;
      s_valid = statvfs(app.cur_tab().current_path.c_str(), &sv) == 0;
      if (s_valid) s_free = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
      s_path = app.cur_tab().current_path;
      s_at = now;
    }
    if (s_valid) free_str = format_size(s_free) + " free";
  }

  cairo_text_extents_t te;
  cairo_text_extents(cr, free_str.c_str(), &te);
  double free_w = te.width;

  if (!free_str.empty()) {
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 1.0);
    cairo_move_to(cr, static_cast<double>(w) - pad - free_w, y + status_h / 2 + 4);
    cairo_show_text(cr, free_str.c_str());
  }

  // ── Zoom slider (levels 0..16) ──
  {
    int track_w = std::min(140, static_cast<int>(140.0 * zf));
    int btn_sz = 18;
    int cy = y + status_h / 2;
    int track_x = w - pad - free_w - static_cast<int>(24.0 * zf) - btn_sz * 2 - 12 - track_w;
    if (track_x > pad + 200 * zf) {
      auto dim = [&](bool hov) {
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, hov ? 0.9 : 0.55);
      };
      // minus glyph
      dim(false);
      cairo_set_line_width(cr, 1.6);
      cairo_move_to(cr, track_x - 14, cy + 0.5);
      cairo_line_to(cr, track_x - 14 + btn_sz - 4, cy + 0.5);
      cairo_stroke(cr);
      // plus glyph
      int px = track_x + track_w + 10;
      cairo_move_to(cr, px, cy + 0.5);
      cairo_line_to(cr, px + btn_sz - 4, cy + 0.5);
      cairo_stroke(cr);
      cairo_move_to(cr, px + (btn_sz - 4) / 2.0, cy - (btn_sz - 4) / 2.0 + 0.5);
      cairo_line_to(cr, px + (btn_sz - 4) / 2.0, cy + (btn_sz - 4) / 2.0 + 0.5);
      cairo_stroke(cr);
      // track
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.45);
      cairo_rectangle(cr, track_x, cy - 2, track_w, 3);
      cairo_fill(cr);
      // handle at current level
      double t = static_cast<double>(zoom_level_for_pct(app.settings_zoom_pct)) /
                 (kZoomLevelCount - 1);
      double hx = track_x + t * (track_w - 10);
      bool hov = app.status_zoom_dragging;
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                            hov ? 1.0 : 0.9);
      cairo_arc(cr, hx + 5, cy, 5, 0, 2 * M_PI);
      cairo_fill(cr);
      app.status_zoom_slider_x = track_x;
      app.status_zoom_slider_w = track_w;
    } else {
      app.status_zoom_slider_x = 0;
      app.status_zoom_slider_w = 0;
    }
  }
}

// ── directory picker bar ─────────────────────────────────────────

void draw_select_dir_bar(AppState& app, cairo_t* cr, int w, int h,
                         int bar_h) {
  double zf = app.zoom_pct / 100.0;
  int y = h - app.status_bar_height - bar_h;
  app.select_bar_y = y;
  double sa = app.statusbar_opacity_pct / 100.0;

  // Background
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, sa);
  cairo_rectangle(cr, 0, y, w, bar_h);
  cairo_fill(cr);

  // border-t
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  cairo_rectangle(cr, 0, y, w, 1);
  cairo_fill(cr);

  int pad = static_cast<int>(24.0 * zf);

  // "Select:" label + path/file
  std::string label;
  if (app.select_file_mode) {
    auto& tab = app.cur_tab();
    if (tab.selected_idx >= 0 && tab.selected_idx < static_cast<int>(tab.visible_entries.size())) {
      auto& fe = tab.entries[tab.visible_entries[tab.selected_idx]];
      label = "Select: " + fe.path;
    } else {
      label = "Select: (select a file)";
    }
  } else {
    label = "Select: " + app.cur_tab().current_path;
  }
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * zf);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
  cairo_move_to(cr, pad, y + bar_h / 2 + 4);
  cairo_show_text(cr, label.c_str());

  // ── Select button ──
  int btn_w = static_cast<int>(80.0 * zf);
  int btn_h = static_cast<int>(28.0 * zf);
  int btn_gap = static_cast<int>(8.0 * zf);
  int sel_x = w - pad - btn_w;
  int can_x = sel_x - btn_gap - btn_w;
  int btn_y = y + (bar_h - btn_h) / 2;

  app.select_btn_x = sel_x;
  app.select_btn_w = btn_w;
  app.cancel_btn_x = can_x;
  app.cancel_btn_w = btn_w;

  // Select button
  double r = 4.0 * zf;
  if (app.select_btn_hover) {
    cairo_set_source_rgba(cr, 0.3, 0.5, 1.0, 1.0);
  } else {
    cairo_set_source_rgba(cr, 0.2, 0.4, 0.9, 1.0);
  }
  draw_rounded_rect(cr, static_cast<double>(sel_x), static_cast<double>(btn_y),
                    static_cast<double>(btn_w), static_cast<double>(btn_h), r);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
  cairo_set_font_size(cr, 13.0 * zf);
  cairo_text_extents_t te;
  cairo_text_extents(cr, "Select", &te);
  cairo_move_to(cr, sel_x + (btn_w - te.width) / 2,
                btn_y + (btn_h - te.height) / 2 - te.y_bearing);
  cairo_show_text(cr, "Select");

  // Cancel button
  if (app.cancel_btn_hover) {
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 1.0);
  } else {
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 1.0);
  }
  draw_rounded_rect(cr, static_cast<double>(can_x), static_cast<double>(btn_y),
                    static_cast<double>(btn_w), static_cast<double>(btn_h), r);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 1.0);
  cairo_text_extents(cr, "Cancel", &te);
  cairo_move_to(cr, can_x + (btn_w - te.width) / 2,
                btn_y + (btn_h - te.height) / 2 - te.y_bearing);
  cairo_show_text(cr, "Cancel");
}

// ── create dialog ────────────────────────────────────────────────

void draw_create_dialog(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int dlg_w = 340;
  int dlg_h = 160;
  int dlg_x = (w - dlg_w) / 2;
  int dlg_y = (h - dlg_h) / 2;
  double sa = app.surface_opacity_pct / 100.0;

  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, sa);
  draw_rounded_rect(cr, dlg_x, dlg_y, dlg_w, dlg_h, 10);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, dlg_x + 0.5, dlg_y + 0.5, dlg_w - 1, dlg_h - 1, 9.5);
  cairo_stroke(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, dlg_x + 20, dlg_y + 30);
  cairo_show_text(cr, "New Folder");

  int input_x = dlg_x + 20;
  int input_y = dlg_y + 50;
  int input_w = dlg_w - 40;
  int input_h = 34;
  cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
  draw_rounded_rect(cr, input_x, input_y, input_w, input_h, 6);
  cairo_fill(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 14);

  // Draw selection highlight
  if (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end) {
    int sel_a = std::min(app.create_sel_start, app.create_sel_end);
    int sel_b = std::max(app.create_sel_start, app.create_sel_end);
    std::string before_sel = app.create_buf.substr(0, static_cast<std::size_t>(sel_a));
    std::string sel_text = app.create_buf.substr(static_cast<std::size_t>(sel_a), static_cast<std::size_t>(sel_b - sel_a));
    cairo_text_extents_t te_before, te_sel;
    cairo_text_extents(cr, before_sel.c_str(), &te_before);
    cairo_text_extents(cr, sel_text.c_str(), &te_sel);
    double sel_x = input_x + 10 + te_before.width;
    double sel_y = input_y + 4;
    double sel_w = te_sel.width;
    double sel_h = static_cast<double>(input_h) - 8;
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.35);
    cairo_rectangle(cr, sel_x, sel_y, sel_w, sel_h);
    cairo_fill(cr);
  }

  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, input_x + 10, input_y + input_h / 2 + 4);
  cairo_show_text(cr, app.create_buf.c_str());

  if (app.create_buf.empty()) {
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, input_x + 10, input_y + input_h / 2 + 4);
    cairo_show_text(cr, "Folder name");
  } else {
    cairo_text_extents_t te;
    cairo_text_extents(cr, app.create_buf.substr(0, app.create_cursor_pos).c_str(), &te);
    int cx = input_x + 10 + static_cast<int>(te.width);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.7);
    cairo_rectangle(cr, cx, input_y + 6, 1, input_h - 12);
    cairo_fill(cr);
  }

  int btn_y = dlg_y + dlg_h - 50;
  int btn_w = 90;
  int btn_h = 32;
  int cancel_x = dlg_x + dlg_w - 220;
  int create_x = dlg_x + dlg_w - 110;

  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 0.6);
  draw_rounded_rect(cr, cancel_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, cancel_x + btn_w / 2 - 16, btn_y + btn_h / 2 + 4);
  cairo_show_text(cr, "Cancel");

  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
  draw_rounded_rect(cr, create_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, create_x + btn_w / 2 - 16, btn_y + btn_h / 2 + 4);
  cairo_show_text(cr, "Create");
}

// ── confirm dialog ───────────────────────────────────────────────

void draw_confirm_dialog(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int dlg_w = 380;
  int dlg_h = 170;
  int dlg_x = (w - dlg_w) / 2;
  int dlg_y = (h - dlg_h) / 2;
  double sa = app.surface_opacity_pct / 100.0;

  // Backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Card
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, sa);
  draw_rounded_rect(cr, dlg_x, dlg_y, dlg_w, dlg_h, 10);
  cairo_fill(cr);

  // Border
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, dlg_x + 0.5, dlg_y + 0.5, dlg_w - 1, dlg_h - 1, 9.5);
  cairo_stroke(cr);

  // Title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, dlg_x + 20, dlg_y + 30);
  cairo_show_text(cr, app.confirm_title.c_str());

  // File-type icon
  int icon_x = dlg_x + 20;
  int icon_y = dlg_y + 50;
  int icon_sz = 48;

  if (app.confirm_item_count == 1 && !app.confirm_preview_path.empty()) {
    FileType ft = FileType::File;
    auto dot = app.confirm_preview_path.rfind('.');
    if (dot != std::string::npos) {
      std::string ext = app.confirm_preview_path.substr(dot + 1);
      for (auto& c : ext) c = static_cast<char>(std::tolower(c));
      if (ext == "png" || ext == "jpg" || ext == "jpeg" ||
          ext == "gif" || ext == "bmp" || ext == "webp" ||
          ext == "svg" || ext == "avif" || ext == "tif" ||
          ext == "tiff" || ext == "psd" || ext == "xcf" ||
           ext == "ai" || ext == "eps" || ext == "af" || ext == "afphoto" ||
           ext == "afdesign" || ext == "afpub" || ext == "face" || ext == "icon")
        ft = FileType::Image;
      else if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "bz2" ||
               ext == "xz" || ext == "7z" || ext == "rar" || ext == "zst" ||
               ext == "zstd" || ext == "iso" || ext == "cab" || ext == "dmg")
        ft = FileType::Archive;
      else if (ext == "sh" || ext == "bin" || ext == "elf" || ext == "exe" ||
               ext == "desktop" || ext == "deb" || ext == "rpm" ||
               ext == "AppImage" || ext == "appimage" || ext == "flatpak" ||
               ext == "snap" || ext == "run" || ext == "msi")
        ft = FileType::Executable;
      else if (ext == "html" || ext == "htm" || ext == "xhtml" ||
               ext == "css" || ext == "php" || ext == "wasm")
        ft = FileType::Web;
      else if (ext == "md" || ext == "markdown" || ext == "mdown" || ext == "mkd")
        ft = FileType::Markdown;
      else if (ext == "c" || ext == "cpp" || ext == "h" || ext == "hpp" ||
               ext == "py" || ext == "rs" || ext == "go" || ext == "java" ||
               ext == "js" || ext == "ts" || ext == "rb")
        ft = FileType::Code;
      else if (ext == "pdf" || ext == "doc" || ext == "docx" ||
               ext == "xls" || ext == "xlsx" || ext == "ppt" || ext == "pptx" ||
               ext == "odt" || ext == "ods" || ext == "odp" || ext == "rtf" ||
               ext == "epub" || ext == "djvu")
        ft = FileType::Document;
      else if (ext == "ttf" || ext == "otf" || ext == "woff" || ext == "woff2")
        ft = FileType::Font;
      else if (ext == "txt" || ext == "conf" || ext == "json" ||
               ext == "xml" || ext == "log" || ext == "yaml" ||
               ext == "yml" || ext == "toml" || ext == "ini" || ext == "cfg")
        ft = FileType::Text;
      else if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg" ||
               ext == "m4a" || ext == "aac" || ext == "opus" || ext == "wma")
        ft = FileType::Audio;
      else if (ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov" ||
               ext == "webm")
        ft = FileType::Video;
    }
    const auto* ic = app.icons.tray_icon(icon_name_for_file_type(ft, &app.confirm_preview_path));
    if (ic && ic->surface) {
      double iw = static_cast<double>(ic->width);
      double ih = static_cast<double>(ic->height);
      if (iw > 0 && ih > 0) {
        double scale = icon_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_translate(cr, icon_x, icon_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, ic->surface,
                                 (icon_sz / scale - iw) / 2,
                                 (icon_sz / scale - ih) / 2);
        cairo_paint(cr);
        cairo_restore(cr);
      }
    }
  }

  // Message text
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_text_extents_t te;
  int text_x = icon_x + icon_sz + 12;
  int text_max_w = dlg_x + dlg_w - 20 - text_x;
  std::string msg = app.confirm_message;
  if (cairo_text_extents(cr, msg.c_str(), &te),
      te.width > static_cast<double>(text_max_w)) {
    while (!msg.empty()) {
      msg.pop_back();
      cairo_text_extents(cr, (msg + "\u2026").c_str(), &te);
      if (te.width <= static_cast<double>(text_max_w)) {
        msg += "\u2026";
        break;
      }
    }
  }
  cairo_text_extents(cr, msg.c_str(), &te);
  int text_y = icon_y + icon_sz / 2 + static_cast<int>(te.height) / 2;
  cairo_move_to(cr, text_x, text_y);
  cairo_show_text(cr, msg.c_str());

  // Buttons
  int btn_y = dlg_y + dlg_h - 50;
  int btn_w = 90;
  int btn_h = 32;
  int cancel_x = dlg_x + dlg_w - 220;
  int delete_x = dlg_x + dlg_w - 110;

  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 0.6);
  draw_rounded_rect(cr, cancel_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, cancel_x + btn_w / 2 - 20, btn_y + btn_h / 2 + 4);
  cairo_show_text(cr, "Cancel");

  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.90);
  draw_rounded_rect(cr, delete_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, delete_x + btn_w / 2 - 20, btn_y + btn_h / 2 + 4);
  cairo_show_text(cr, "Delete");

  // Hover highlight
  if (app.confirm_hover_btn >= 0) {
    int hx = app.confirm_hover_btn == 0 ? cancel_x : delete_x;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.12);
    draw_rounded_rect(cr, hx, btn_y, btn_w, btn_h, 6);
    cairo_fill(cr);
  }
}

// ── Overwrite/merge conflict dialog (Dolphin-style) ─────────────

void draw_conflict_dialog(AppState& app, cairo_t* cr) {
  if (app.conflict_queue.empty()) return;
  const auto& c = app.conflict_queue.front();

  int w = app.width;
  int h = app.height;
  int dlg_w = 520;
  int dlg_h = 320;
  int dlg_x = (w - dlg_w) / 2;
  int dlg_y = (h - dlg_h) / 2;
  double sa = app.surface_opacity_pct / 100.0;

  // Backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.40);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Card
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, sa);
  draw_rounded_rect(cr, dlg_x, dlg_y, dlg_w, dlg_h, 12);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.30);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, dlg_x + 0.5, dlg_y + 0.5, dlg_w - 1, dlg_h - 1, 11.5);
  cairo_stroke(cr);

  std::string name = fs::path(c.src).filename().string();
  bool merge = c.src_is_dir && c.dest_is_dir;

  // Title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  {
    std::string title = merge ? "Merge folder \"" + name + "\"?" :
                        (c.src_is_dir ? "Replace folder \"" + name + "\"?"
                                      : "Replace file \"" + name + "\"?");
    cairo_move_to(cr, dlg_x + 22, dlg_y + 32);
    cairo_show_text(cr, title.c_str());
  }

  // Subtitle
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
  {
    const char* sub = merge
        ? "The destination contains a folder with the same name. Files with the same name will be replaced."
        : (c.src_is_dir ? "The destination contains a file where the source has a folder."
                        : "The destination contains a file with the same name. Overwriting will replace its contents.");
    cairo_move_to(cr, dlg_x + 22, dlg_y + 52);
    cairo_show_text(cr, sub);
  }

  // ── Source vs Destination columns ──
  auto fmt_time = [](int64_t sec) {
    if (sec == 0) return std::string("\u2014");
    time_t t = static_cast<time_t>(sec);
    struct tm* tm_local = localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm_local);
    return std::string(buf);
  };
  auto fmt_size = [](uint64_t sz, bool is_dir) {
    if (is_dir) return std::string("Folder");
    char buf[64];
    double v = static_cast<double>(sz);
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int ui = 0;
    while (v >= 1024.0 && ui < 4) { v /= 1024.0; ++ui; }
    if (ui == 0) snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)sz);
    else snprintf(buf, sizeof(buf), "%.1f %s", v, units[ui]);
    return std::string(buf);
  };
  auto draw_column = [&](int x, int y, int col_w, const char* header,
                         const std::string& p, bool is_dir,
                         uint64_t sz, int64_t mtime) {
    // Header pill
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_text_extents_t te;
    cairo_text_extents(cr, header, &te);
    cairo_move_to(cr, x + (col_w - te.x_advance) / 2, y);
    cairo_show_text(cr, header);

    // Icon
    int icon_sz = 44;
    const auto* ic = app.icons.tray_icon(is_dir ? "folder"
                                        : icon_name_for_file_type(FileType::File, &p));
    if (ic && ic->surface) {
      double iw = static_cast<double>(ic->width);
      double ih = static_cast<double>(ic->height);
      if (iw > 0 && ih > 0) {
        double scale = icon_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_translate(cr, x + (col_w - icon_sz) / 2, y + 8);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, ic->surface,
                                 (icon_sz / scale - iw) / 2,
                                 (icon_sz / scale - ih) / 2);
        cairo_paint(cr);
        cairo_restore(cr);
      }
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    std::string line1 = fmt_size(sz, is_dir);
    cairo_text_extents(cr, line1.c_str(), &te);
    cairo_move_to(cr, x + (col_w - te.x_advance) / 2, y + icon_sz + 28);
    cairo_show_text(cr, line1.c_str());

    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    std::string line2 = fmt_time(mtime);
    cairo_text_extents(cr, line2.c_str(), &te);
    cairo_move_to(cr, x + (col_w - te.x_advance) / 2, y + icon_sz + 46);
    cairo_show_text(cr, line2.c_str());
  };

  int col_top = dlg_y + 68;
  int col_w = 200;
  draw_column(dlg_x + 22, col_top, col_w, "SOURCE", c.src, c.src_is_dir, c.src_size, c.src_mtime);
  draw_column(dlg_x + dlg_w - 22 - col_w, col_top, col_w, "DESTINATION",
              c.dest, c.dest_is_dir, c.dest_size, c.dest_mtime);
  // Divider between columns
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, dlg_x + dlg_w / 2 + 0.5, col_top + 4);
  cairo_line_to(cr, dlg_x + dlg_w / 2 + 0.5, col_top + 92);
  cairo_stroke(cr);

  // ── Apply-to-all checkbox (only when more than one conflict remains) ──
  int check_y = dlg_y + dlg_h - 96;
  if (app.conflict_queue.size() > 1) {
    int box_sz = 16;
    int box_x = dlg_x + 22;
    app.conflict_check_rect[0] = box_x;
    app.conflict_check_rect[1] = check_y;
    app.conflict_check_rect[2] = box_sz + 220;
    app.conflict_check_rect[3] = box_sz;
    bool hov = app.conflict_check_hover ||
               (app.pointerX >= box_x && app.pointerX < box_x + box_sz + 220 &&
                app.pointerY >= check_y && app.pointerY < check_y + box_sz);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b,
                          hov ? 0.55 : 0.35);
    cairo_set_line_width(cr, 1.5);
    draw_rounded_rect(cr, box_x, check_y, box_sz, box_sz, 4);
    cairo_stroke(cr);
    if (app.conflict_apply_all) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.9);
      draw_rounded_rect(cr, box_x, check_y, box_sz, box_sz, 4);
      cairo_fill(cr);
      // Checkmark
      cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
      cairo_set_line_width(cr, 2);
      cairo_move_to(cr, box_x + 4, check_y + 8);
      cairo_line_to(cr, box_x + 7, check_y + 11);
      cairo_line_to(cr, box_x + 12, check_y + 5);
      cairo_stroke(cr);
    }
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    std::string label = "Apply this action to all " +
                        std::to_string(app.conflict_queue.size() - 1) + " remaining";
    cairo_move_to(cr, box_x + box_sz + 8, check_y + 13);
    cairo_show_text(cr, label.c_str());
  } else {
    app.conflict_check_rect[0] = 0; app.conflict_check_rect[1] = -100;
    app.conflict_check_rect[2] = 0; app.conflict_check_rect[3] = 0;
  }

  // ── Buttons: Skip | Cancel | Overwrite(Merge) ──
  int btn_y = dlg_y + dlg_h - 54;
  int btn_h = 34;
  int btn_w = 104;
  int b2_x = dlg_x + dlg_w - 22 - btn_w;                    // Overwrite/Merge
  int b1_x = b2_x - 8 - btn_w;                              // Cancel
  int b0_x = b1_x - 8 - btn_w;                              // Skip
  double rects[3][4] = {
    {static_cast<double>(b0_x), static_cast<double>(btn_y), static_cast<double>(btn_w), static_cast<double>(btn_h)},
    {static_cast<double>(b1_x), static_cast<double>(btn_y), static_cast<double>(btn_w), static_cast<double>(btn_h)},
    {static_cast<double>(b2_x), static_cast<double>(btn_y), static_cast<double>(btn_w), static_cast<double>(btn_h)},
  };
  for (int b = 0; b < 3; ++b)
    for (int k = 0; k < 4; ++k) app.conflict_btn_rects[b][k] = rects[b][k];

  const char* labels[3] = {"Skip", "Cancel", merge ? "Merge" : "Overwrite"};
  int centers[3] = {b0_x, b1_x, b2_x};
  for (int b = 0; b < 3; ++b) {
    bool primary = (b == 2);
    bool hov = (app.pointerX >= rects[b][0] && app.pointerX < rects[b][0] + rects[b][2] &&
                app.pointerY >= rects[b][1] && app.pointerY < rects[b][1] + rects[b][3]);
    if (primary) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                            hov ? 1.0 : 0.90);
    } else {
      cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, hov ? 0.85 : 0.6);
    }
    draw_rounded_rect(cr, centers[b], btn_y, btn_w, btn_h, 6);
    cairo_fill(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_text_extents_t te;
    cairo_text_extents(cr, labels[b], &te);
    cairo_move_to(cr, centers[b] + (btn_w - te.x_advance) / 2, btn_y + btn_h / 2 + 4);
    cairo_show_text(cr, labels[b]);
  }
}

// ── password dialog ─────────────────────────────────────────────

void draw_password_dialog(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int card_w = 400;
  int card_h = 210;
  int cx = (w - card_w) / 2;
  int cy = (h - card_h) / 2;
  int pad = 24;
  int card_r = 14;

  // Backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.40);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Drop shadow
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  draw_rounded_rect(cr, cx + 2, cy + 3, card_w, card_h, card_r);
  cairo_fill(cr);

  // Card background
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 1.0);
  draw_rounded_rect(cr, cx, cy, card_w, card_h, card_r);
  cairo_fill_preserve(cr);

  // Card border
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  // Lock icon (Unicode lock symbol)
  {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 18);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.9);
    cairo_move_to(cr, cx + pad, cy + pad + 15);
    cairo_show_text(cr, "\xF0\x9F\x94\x92");
  }

  // Title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, cx + pad + 28, cy + pad + 15);
  cairo_show_text(cr, "Enter Password");

  // Subtitle with truncated archive name
  {
    std::string fname = fs::path(app.password_archive_path).filename().string();
    if (fname.size() > 42) fname = fname.substr(0, 39) + "\xE2\x80\xA6";
    std::string subtitle = "\"" + fname + "\" is password-protected";

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.5);

    cairo_text_extents_t te;
    cairo_text_extents(cr, subtitle.c_str(), &te);
    double max_w = static_cast<double>(card_w - pad * 2);
    if (te.width > max_w) {
      while (subtitle.size() > 10) {
        subtitle.pop_back();
        cairo_text_extents(cr, (subtitle + "\xE2\x80\xA6").c_str(), &te);
        if (te.width <= max_w) { subtitle += "\xE2\x80\xA6"; break; }
      }
    }
    cairo_move_to(cr, cx + pad, cy + pad + 36);
    cairo_show_text(cr, subtitle.c_str());
  }

  // Separator
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.18);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, cx + pad, cy + pad + 50);
  cairo_line_to(cr, cx + card_w - pad, cy + pad + 50);
  cairo_stroke(cr);

  // Input field
  int input_x = cx + pad;
  int input_y = cy + pad + 62;
  int input_w = card_w - pad * 2;
  int input_h = 36;
  int input_r = 8;

  // Input background
  cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.55);
  draw_rounded_rect(cr, input_x, input_y, input_w, input_h, input_r);
  cairo_fill(cr);

  // Input inner glassy rim
  {
    cairo_pattern_t* rim = cairo_pattern_create_linear(0, input_y, 0, input_y + input_h);
    cairo_pattern_add_color_stop_rgba(rim, 0.00, app.text_r, app.text_g, app.text_b, 0.14);
    cairo_pattern_add_color_stop_rgba(rim, 0.30, app.text_r, app.text_g, app.text_b, 0.06);
    cairo_pattern_add_color_stop_rgba(rim, 1.00, app.text_r, app.text_g, app.text_b, 0.04);
    cairo_set_source(cr, rim);
    cairo_set_line_width(cr, 1.0);
    draw_rounded_rect(cr, input_x + 1.5, input_y + 1.5, input_w - 3.0, input_h - 3.0,
                      static_cast<double>(input_r) - 0.5);
    cairo_stroke(cr);
    cairo_pattern_destroy(rim);
  }

  // Password text (masked bullets)
  int text_y = input_y + input_h / 2;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 14);

  if (!app.password_buf.empty()) {
    std::string masked(app.password_buf.size(), '*');

    // Draw selection highlight
    if (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end) {
      int sel_a = std::min(app.password_sel_start, app.password_sel_end);
      int sel_b = std::max(app.password_sel_start, app.password_sel_end);
      std::string before_sel(app.password_buf.begin(), app.password_buf.begin() + std::min(sel_a, static_cast<int>(app.password_buf.size())));
      std::string sel_masked(before_sel.size(), '*');
      std::string sel_part(sel_b - sel_a, '*');
      cairo_text_extents_t te_before, te_sel;
      cairo_text_extents(cr, sel_masked.c_str(), &te_before);
      cairo_text_extents(cr, sel_part.c_str(), &te_sel);
      double sel_x = input_x + 14 + te_before.width;
      double sel_y = static_cast<double>(input_y) + 4;
      double sel_w = te_sel.width;
      double sel_h = static_cast<double>(input_h) - 8;
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.35);
      cairo_rectangle(cr, sel_x, sel_y, sel_w, sel_h);
      cairo_fill(cr);
    }

    cairo_text_extents_t te;
    cairo_text_extents(cr, masked.c_str(), &te);
    double tx = input_x + 14;
    double ty = text_y + te.height * 0.35;
    // Clip to input bounds
    cairo_save(cr);
    draw_rounded_rect(cr, input_x + 2, input_y + 2, input_w - 4, input_h - 4,
                      static_cast<double>(input_r) - 1);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.95);
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, masked.c_str());
    cairo_restore(cr);
  } else {
    // Placeholder
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.30);
    cairo_text_extents_t te;
    cairo_text_extents(cr, "Password", &te);
    cairo_move_to(cr, input_x + 14, text_y + te.height * 0.35);
    cairo_show_text(cr, "Password");
  }

  // Cursor
  {
    std::string before_cursor(app.password_buf.begin(),
                              app.password_buf.begin() + std::min(app.password_cursor_pos,
                                                                   static_cast<int>(app.password_buf.size())));
    std::string masked_before(before_cursor.size(), '*');
    cairo_text_extents_t te;
    cairo_text_extents(cr, masked_before.c_str(), &te);
    int cur_x = input_x + 14 + static_cast<int>(te.width);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.6);
    cairo_rectangle(cr, cur_x, input_y + 8, 1, input_h - 16);
    cairo_fill(cr);
  }

  // Buttons (pill-shaped)
  int btn_h = 32;
  int btn_w = 90;
  int btn_gap = 10;
  int btns_total = btn_w * 2 + btn_gap;
  int btns_x = cx + (card_w - btns_total) / 2;
  int btn_y = cy + card_h - pad - btn_h;

  // Cancel button
  {
    cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.42);
    draw_rounded_rect(cr, btns_x, btn_y, btn_w, btn_h, btn_h / 2);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.40);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_text_extents_t te;
    cairo_text_extents(cr, "Cancel", &te);
    cairo_move_to(cr, btns_x + (btn_w - te.x_advance) / 2,
                  btn_y + btn_h / 2 + te.height * 0.35);
    cairo_show_text(cr, "Cancel");
  }

  // Extract button (accent)
  {
    int ex_x = btns_x + btn_w + btn_gap;
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.15);
    draw_rounded_rect(cr, ex_x, btn_y, btn_w, btn_h, btn_h / 2);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_text_extents_t te;
    cairo_text_extents(cr, "Extract", &te);
    cairo_move_to(cr, ex_x + (btn_w - te.x_advance) / 2,
                  btn_y + btn_h / 2 + te.height * 0.35);
    cairo_show_text(cr, "Extract");
  }
}

// ── compress dialog ──────────────────────────────────────────────

void draw_compress_dialog(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int dlg_w = 420;
  int dlg_h = 310;
  int dlg_x = (w - dlg_w) / 2;
  int dlg_y = (h - dlg_h) / 2;

  // Backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Card (always fully opaque)
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 1.0);
  draw_rounded_rect(cr, dlg_x, dlg_y, dlg_w, dlg_h, 10);
  cairo_fill(cr);

  // Border
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, dlg_x + 0.5, dlg_y + 0.5, dlg_w - 1, dlg_h - 1, 9.5);
  cairo_stroke(cr);

  // Title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, dlg_x + 20, dlg_y + 28);
  cairo_show_text(cr, "Compress");

  // ── Format row ──
  static const char* fmt_labels[] = {"Zip", "Tar.gz", "Tar.bz2", "Tar.xz",
                                      "7z", "Rar", "Tar"};
  int content_x = dlg_x + 20;
  int content_y = dlg_y + 50;
  int fmt_w = 80;
  int fmt_h = 28;
  int fmt_gap = 8;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  cairo_move_to(cr, content_x, content_y);
  cairo_show_text(cr, "Format");

  int fmy = content_y + 18;
  for (int i = 0; i < 4; ++i) {
    int fmx = content_x + i * (fmt_w + fmt_gap);
    bool avail = app.compress_format_available[i];
    double r = 6;
    if (i == app.compress_format) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, avail ? 0.85 : 0.30);
    } else if (i == app.compress_hover_format && avail) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.50);
    } else {
      cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
    }
    draw_rounded_rect(cr, fmx, fmy, fmt_w, fmt_h, r);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, avail ? 1.0 : 0.35);
    cairo_move_to(cr, fmx + 6, fmy + fmt_h / 2 + 4);
    cairo_show_text(cr, fmt_labels[i]);
  }
  int fmy2 = fmy + fmt_h + fmt_gap;
  for (int i = 4; i < 7; ++i) {
    int fmx = content_x + (i - 4) * (fmt_w + fmt_gap);
    bool avail = app.compress_format_available[i];
    double r = 6;
    if (i == app.compress_format) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, avail ? 0.85 : 0.30);
    } else if (i == app.compress_hover_format && avail) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.50);
    } else {
      cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
    }
    draw_rounded_rect(cr, fmx, fmy2, fmt_w, fmt_h, r);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, avail ? 1.0 : 0.35);
    cairo_move_to(cr, fmx + 6, fmy2 + fmt_h / 2 + 4);
    cairo_show_text(cr, fmt_labels[i]);
  }

  // ── Name row ──
  int name_y = fmy2 + fmt_h + 14;
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  cairo_move_to(cr, content_x, name_y);
  cairo_show_text(cr, "Name");

  int input_x = content_x;
  int input_y = name_y + 18;
  int input_w = dlg_w - 40;
  int input_h = 32;
  cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
  draw_rounded_rect(cr, input_x, input_y, input_w, input_h, 6);
  cairo_fill(cr);

  cairo_set_font_size(cr, 13);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, input_x + 8, input_y + input_h / 2 + 4);
  cairo_show_text(cr, app.compress_name_buf.c_str());

  if (app.compress_name_buf.empty()) {
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 0.6);
    cairo_move_to(cr, input_x + 8, input_y + input_h / 2 + 4);
    cairo_show_text(cr, "Archive name");
  } else {
    cairo_text_extents_t te;
    cairo_text_extents(cr, app.compress_name_buf.substr(0, app.compress_name_cursor).c_str(), &te);
    int cx = input_x + 8 + static_cast<int>(te.width);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.7);
    cairo_rectangle(cr, cx, input_y + 6, 1, input_h - 12);
    cairo_fill(cr);
  }

  // ── Level row ──
  int lvl_y = input_y + input_h + 14;
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  cairo_move_to(cr, content_x, lvl_y);
  cairo_show_text(cr, "Level");

  static const char* level_labels[] = {"Fastest", "Fast", "Normal", "Maximum", "Maximal"};
  static constexpr int kNumLevels = 5;
  static constexpr int kLevelValues[kNumLevels] = {0, 3, 6, 8, 9};
  int lvl_btn_x = content_x;
  int lvl_btn_y = lvl_y + 18;
  int lvl_btn_w = 68;
  int lvl_btn_h = 28;
  int lvl_gap = 8;
  for (int i = 0; i < kNumLevels; ++i) {
    int lx = lvl_btn_x + i * (lvl_btn_w + lvl_gap);
    double r = 6;
    if (kLevelValues[i] == app.compress_level) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
    } else if (i == app.compress_hover_level) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.50);
    } else {
      cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
    }
    draw_rounded_rect(cr, lx, lvl_btn_y, lvl_btn_w, lvl_btn_h, r);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 11);
    cairo_move_to(cr, lx + 4, lvl_btn_y + lvl_btn_h / 2 + 4);
    cairo_show_text(cr, level_labels[i]);
  }

  // ── Buttons ──
  int btn_y = dlg_y + dlg_h - 50;
  int btn_w = 90;
  int btn_h = 32;
  int cancel_x = dlg_x + dlg_w - 220;
  int compress_x = dlg_x + dlg_w - 110;

  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 0.6);
  draw_rounded_rect(cr, cancel_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, cancel_x + btn_w / 2 - 20, btn_y + btn_h / 2 + 4);
  cairo_show_text(cr, "Cancel");

  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
  draw_rounded_rect(cr, compress_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, compress_x + btn_w / 2 - 24, btn_y + btn_h / 2 + 4);
  cairo_show_text(cr, "Compress");

  // Hover highlight
  if (app.compress_hover_btn >= 0) {
    int hx = app.compress_hover_btn == 0 ? cancel_x : compress_x;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.12);
    draw_rounded_rect(cr, hx, btn_y, btn_w, btn_h, 6);
    cairo_fill(cr);
  }
}

// ── terminal chooser dialog ──────────────────────────────────────

void draw_terminal_chooser(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;

  const int kPad = 20;
  const int kTopBarH = 44;
  const int kEntryH = 40;
  const int kBottomBarH = 52;
  const int kCardRad = 12;
  const int kMaxListH = 300;

  const int total = static_cast<int>(app.term_chooser_apps.size());
  const int max_visible = std::max(1, kMaxListH / kEntryH);
  const int visible = std::min(total, max_visible);
  const int list_h = visible * kEntryH;
  const int card_w = 400;
  const int card_h = kPad + kTopBarH + 8 + list_h + 8 + kBottomBarH + kPad;
  const int card_x = (w - card_w) / 2;
  const int card_y = (h - card_h) / 2;

  app.term_chooser_x = card_x;
  app.term_chooser_y = card_y;
  app.term_chooser_w = card_w;

  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  double sa = app.surface_opacity_pct / 100.0;
  cairo_set_source_rgba(cr, 0, 0, 0, 0.28);
  draw_rounded_rect(cr, card_x + 3, card_y + 4, card_w, card_h, kCardRad);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, sa);
  draw_rounded_rect(cr, card_x, card_y, card_w, card_h, kCardRad);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  const int close_x = card_x + card_w - kPad - 28;
  const int close_y = card_y + kPad - 4;
  {
    int cHov = (app.term_chooser_hover == -2) ? 1 : 0;
    double a = cHov ? 0.55 : 0.40;
    cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, a);
    draw_rounded_rect(cr, close_x, close_y, 28, 28, 6);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, close_x + 9, close_y + 9);
    cairo_line_to(cr, close_x + 19, close_y + 19);
    cairo_move_to(cr, close_x + 19, close_y + 9);
    cairo_line_to(cr, close_x + 9, close_y + 19);
    cairo_stroke(cr);
  }

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, card_x + kPad, card_y + kPad + 17);
  cairo_show_text(cr, "Choose Terminal");

  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.15);
  cairo_rectangle(cr, card_x, card_y + kPad + kTopBarH, card_w, 1);
  cairo_fill(cr);

  const int list_x = card_x + 12;
  const int list_y = card_y + kPad + kTopBarH + 8;
  const int list_w = card_w - 24;

  cairo_save(cr);
  cairo_rectangle(cr, list_x, list_y, list_w, list_h);
  cairo_clip(cr);

  const int start = app.term_chooser_scroll;
  const int end = std::min(start + visible, total);
  for (int i = start; i < end; ++i) {
    const int ey = list_y + (i - start) * kEntryH;
    const bool hov = (i == app.term_chooser_hover);

    if (hov) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
      cairo_rectangle(cr, list_x, ey, list_w, kEntryH);
      cairo_fill(cr);
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, list_x + 8, ey + kEntryH / 2 + 5);
    cairo_show_text(cr, app.term_chooser_apps[i].name.c_str());

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.08);
    cairo_move_to(cr, list_x + 8, ey + kEntryH - 0.5);
    cairo_line_to(cr, list_x + list_w - 8, ey + kEntryH - 0.5);
    cairo_set_line_width(cr, 0.5);
    cairo_stroke(cr);
  }

  cairo_restore(cr);

  if (total > visible) {
    const double sbTrackH = list_h;
    const double sbH = std::max(6.0, sbTrackH * visible / static_cast<double>(total));
    const double sbMax = sbTrackH - sbH;
    const double frac = sbMax > 0
        ? static_cast<double>(app.term_chooser_scroll) /
              static_cast<double>(total - visible)
        : 0.0;
    const double sbY = list_y + frac * sbMax;
    const double sx = list_x + list_w - 8;
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, sx, sbY, 4, sbH, 2);
    cairo_fill(cr);
  }
}

// ── marquee / rubber-band selection ──────────────────────────────

void draw_marquee(AppState& app, cairo_t* cr) {
  double x0 = app.marquee_x0;
  double y0 = app.marquee_y0;
  double x1 = app.marquee_x1;
  double y1 = app.marquee_y1;
  double mx = std::min(x0, x1);
  double my = std::min(y0, y1);
  double mw = std::abs(x1 - x0);
  double mh = std::abs(y1 - y0);
  if (mw < 2.0 || mh < 2.0) return;

  // Fill
  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.14);
  cairo_rectangle(cr, mx, my, mw, mh);
  cairo_fill(cr);

  // Dashed border
  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
  cairo_set_line_width(cr, 1.0);
  const double dashes[] = {4.0, 4.0};
  cairo_set_dash(cr, dashes, 2, 0.0);
  cairo_rectangle(cr, mx + 0.5, my + 0.5, mw - 1.0, mh - 1.0);
  cairo_stroke(cr);
  cairo_set_dash(cr, nullptr, 0, 0.0);
}

void hit_test_marquee(AppState& app) {
  double x0 = std::min(app.marquee_x0, app.marquee_x1);
  double y0 = std::min(app.marquee_y0, app.marquee_y1);
  double x1 = std::max(app.marquee_x0, app.marquee_x1);
  double y1 = std::max(app.marquee_y0, app.marquee_y1);

  int sidebar_w = app.sidebar_expanded ? app.sidebar_width : 0;
  int content_x = sidebar_w;
  int content_w = app.width - sidebar_w;
  int content_y = app.top_bar_height + app.tab_bar_height;

  // Split pane adjustment
  if (app.split_view) {
    int div_w = 4;
    int split = app.split_divider_x;
    if (split <= 0) split = content_w / 2;
    if (app.active_pane == 1) {
      int right_x = std::min(content_x + content_w - 100, content_x + split + div_w / 2);
      content_w = content_x + content_w - right_x;
      content_x = right_x;
    } else {
      content_w = std::max(100, split - div_w / 2);
    }
    content_y += app.top_bar_height; // pane top bar
  }

  app.cur_tab().multi_selected.clear();
  app.cur_tab().selected_idx = -1;

  if (app.cur_tab().view_mode == ViewMode::List) {
    int entry_h = app.entry_height;
    int header_h = static_cast<int>(entry_h * 0.55);
    bool grouped = app.cur_tab().group_by_type;
    int prev_type = -1;
    int acc = 0;
    for (int i = 0; i < static_cast<int>(app.cur_tab().visible_entries.size()); ++i) {
      if (grouped) {
        int r = app.cur_tab().visible_entries[i];
        if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size())) {
          int t = static_cast<int>(app.cur_tab().entries[r].type);
          if (t != prev_type) { acc += header_h; prev_type = t; }
        }
      }
      double iy = static_cast<double>(content_y - app.cur_tab().scroll_px + acc);
      double ih = static_cast<double>(entry_h);
      if (iy > y1) break;
      if (iy + ih >= y0 && !(x1 < content_x || x0 > content_x + content_w)) {
        app.cur_tab().multi_selected.push_back(i);
        if (app.cur_tab().selected_idx < 0) app.cur_tab().selected_idx = i;
      }
      acc += entry_h;
    }
  } else if (app.cur_tab().view_mode == ViewMode::Compact) {
    double zf = app.zoom_pct / 100.0;
    int entry_h = static_cast<int>(24.0 * zf);
    for (int i = 0; i < static_cast<int>(app.cur_tab().visible_entries.size()); ++i) {
      double iy = static_cast<double>(content_y - app.cur_tab().scroll_px + i * entry_h);
      double ih = static_cast<double>(entry_h);
      if (iy + ih < y0) continue;
      if (iy > y1) break;
      if (x1 < content_x || x0 > content_x + content_w) continue;
      app.cur_tab().multi_selected.push_back(i);
      if (app.cur_tab().selected_idx < 0) app.cur_tab().selected_idx = i;
    }
  } else if (app.cur_tab().view_mode == ViewMode::Grid) {
    double zf = app.zoom_pct / 100.0;
    int col_gap = app.grid_cell_gap;
    int row_gap = static_cast<int>(10.0 * zf);
    int cell_w = app.grid_cell_size;
    int cols = app.grid_cols;
    int row_h = app.grid_row_h;

    // Must mirror draw_grid_view exactly: icon_size, item_h, and the
    // horizontal centering offset all factor into cell placement.
    int icon_size = std::min(cell_w - static_cast<int>(16.0 * zf),
                             static_cast<int>(72.0 * zf));
    int text_gap = static_cast<int>(4.0 * zf);
    int label_h = static_cast<int>(32.0 * zf); // 2 lines of label text
    int item_h = icon_size + text_gap + label_h;

    int grid_w = cols * cell_w + (cols - 1) * col_gap;
    int grid_offset_x = (content_w - grid_w) / 2;

    int y = content_y + row_gap - app.cur_tab().scroll_px;

    double clamp_x1 = std::min(x1, static_cast<double>(content_x + content_w));
    for (int i = 0; i < static_cast<int>(app.cur_tab().visible_entries.size()); ++i) {
      int col = i % cols;
      int row = i / cols;
      double cx = static_cast<double>(content_x + grid_offset_x + col * (cell_w + col_gap));
      double cy = static_cast<double>(y + row * row_h);
      double cw = static_cast<double>(cell_w);
      double ch = static_cast<double>(item_h);
      if (cy + ch < y0) continue;
      if (cy > y1) break;
      if (cx + cw < x0 || cx > clamp_x1) continue;
      app.cur_tab().multi_selected.push_back(i);
      if (app.cur_tab().selected_idx < 0) app.cur_tab().selected_idx = i;
    }
  }
}

// ── context menu drawing ─────────────────────────────────────────

static void draw_submenu_popup(AppState& app, cairo_t* cr, const std::vector<AppState::ContextMenuItem>& items,
                                int px, int py, int* out_w, int* out_h) {
  int sm_w = 200;
  int sm_h = 0;
  for (const auto& item : items) {
    sm_h += (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) ? 9 : 34;
  }

  int sm_x = px;
  int sm_y = py;
  if (sm_y + sm_h > app.height - 8) sm_y = app.height - sm_h - 8;
  if (sm_y < 8) sm_y = 8;

  // Drop shadow (3 layers)
  for (int s = 3; s >= 0; --s) {
    double a = 0.12 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, sm_x + s * 2.5, sm_y + s * 3, sm_w, sm_h, 10);
    cairo_fill(cr);
  }

  // Card background
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 1.0);
  draw_rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 10);
  cairo_fill_preserve(cr);

  // Border
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.30);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  int ry = sm_y;
  for (size_t i = 0; i < items.size(); ++i) {
    const auto& item = items[i];
    if (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) {
      ry += 4;
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.18);
      cairo_set_line_width(cr, 1);
      cairo_move_to(cr, sm_x + 14, ry + 0.5);
      cairo_line_to(cr, sm_x + sm_w - 14, ry + 0.5);
      cairo_stroke(cr);
      ry += 5;
      continue;
    }

    int row_h = 34;
    bool hovered = (static_cast<int>(i) == app.context_menu_sub_hover);

    // Hover highlight
    if (hovered) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.14);
      draw_rounded_rect(cr, sm_x + 5, ry + 2, sm_w - 10, row_h - 4, 6);
      cairo_fill(cr);
    }

    // Label
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_text_extents_t te;
    cairo_text_extents(cr, item.label.c_str(), &te);
    double tx = sm_x + 16;
    double ty = ry + (row_h - te.height) / 2.0 - te.y_bearing;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, item.label.c_str());
    ry += row_h;
  }

  if (out_w) *out_w = sm_w;
  if (out_h) *out_h = sm_h;
}

void draw_context_menu(AppState& app, cairo_t* cr) {
  int cm_x = app.context_menu_x;
  int cm_y = app.context_menu_y;
  int cm_w = 240;
  int cm_h = 0;
  for (const auto& item : app.context_menu_items) {
    cm_h += (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) ? 9 : 34;
  }

  // Clamp to screen
  if (cm_x + cm_w > app.width - 8) cm_x = app.width - cm_w - 8;
  if (cm_y + cm_h > app.height - 8) cm_y = app.height - cm_h - 8;
  if (cm_x < 8) cm_x = 8;
  if (cm_y < 8) cm_y = 8;

  // Drop shadow (3 layers, heavier)
  for (int s = 3; s >= 0; --s) {
    double a = 0.14 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, cm_x + s * 2.5, cm_y + s * 3, cm_w, cm_h, 10);
    cairo_fill(cr);
  }

  // Card background
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 1.0);
  draw_rounded_rect(cr, cm_x, cm_y, cm_w, cm_h, 10);
  cairo_fill_preserve(cr);

  // Border
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.30);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  int ry = cm_y;
  for (int i = 0; i < static_cast<int>(app.context_menu_items.size()); ++i) {
    const auto& item = app.context_menu_items[i];
    if (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) {
      ry += 4;
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.18);
      cairo_set_line_width(cr, 1);
      cairo_move_to(cr, cm_x + 14, ry + 0.5);
      cairo_line_to(cr, cm_x + cm_w - 14, ry + 0.5);
      cairo_stroke(cr);
      ry += 5;
      continue;
    }

    int row_h = 34;
    bool has_sub = !item.sub_items.empty();
    bool hovered = (i == app.context_menu_hover);

    // Hover highlight
    if (hovered) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.14);
      draw_rounded_rect(cr, cm_x + 5, ry + 2, cm_w - 10, row_h - 4, 6);
      cairo_fill(cr);
    }

    // Label text with proper vertical centering
    bool destructive = (item.action == AppState::ContextMenuAction::MoveToTrash ||
                        item.action == AppState::ContextMenuAction::PermanentDelete);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            has_sub ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    if (destructive) {
      cairo_set_source_rgba(cr, 0.95, 0.30, 0.30, 0.90);
    } else {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    }
    cairo_text_extents_t te;
    cairo_text_extents(cr, item.label.c_str(), &te);
    double tx = cm_x + 16;
    double ty = ry + (row_h - te.height) / 2.0 - te.y_bearing;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, item.label.c_str());

    // Submenu arrow chevron
    if (has_sub) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.45);
      cairo_set_line_width(cr, 1.5);
      int ax = cm_x + cm_w - 18;
      int ay = ry + row_h / 2;
      cairo_move_to(cr, ax - 1, ay - 4);
      cairo_line_to(cr, ax + 4, ay);
      cairo_line_to(cr, ax - 1, ay + 4);
      cairo_stroke(cr);
    }

    ry += row_h;
  }

  // Draw open submenu
  if (app.context_menu_hover >= 0 &&
      static_cast<size_t>(app.context_menu_hover) < app.context_menu_items.size()) {
    const auto& item = app.context_menu_items[app.context_menu_hover];
    if (!item.sub_items.empty()) {
      int sub_x = cm_x + cm_w + 3;
      int sub_y = cm_y;
      for (int j = 0; j < app.context_menu_hover; ++j) {
        sub_y += (app.context_menu_items[j].action == AppState::ContextMenuAction::Separator && app.context_menu_items[j].sub_items.empty()) ? 9 : 34;
      }
      int sm_w = 0, sm_h = 0;
      draw_submenu_popup(app, cr, item.sub_items, sub_x, sub_y, &sm_w, &sm_h);
    }
  }
}

// ── hit testing ──────────────────────────────────────────────────

int hit_test_list(AppState& app, int x, int y) {
  int sidebar_w = app.sidebar_expanded ? app.sidebar_width : 0;
  int content_x = sidebar_w;
  int content_y = app.top_bar_height + app.tab_bar_height;
  int content_w = app.width - sidebar_w;

  // Split pane adjustment
  if (app.split_view) {
    int div_w = 4;
    int split = app.split_divider_x;
    if (split <= 0) split = content_w / 2;
    if (app.active_pane == 1) {
      int right_x = std::min(content_x + content_w - 100, content_x + split + div_w / 2);
      content_w = content_x + content_w - right_x;
      content_x = right_x;
    } else {
      content_w = std::max(100, split - div_w / 2);
    }
  }

  int view_h = app.height - content_y - app.status_bar_height;
  int col_header_h = app.entry_height;
  int scroll = app.cur_tab().scroll_px;

  if (x < content_x || x >= content_x + content_w) return -1;
  if (y < content_y || y >= content_y + view_h) return -1;

  int rel_y = y - content_y - col_header_h + scroll;
  if (rel_y < 0) return -1;

  if (!app.cur_tab().group_by_type) {
    int idx = rel_y / app.entry_height;
    if (idx < 0 || idx >= static_cast<int>(app.cur_tab().visible_entries.size()))
      return -1;
    return idx;
  }

  int hdr_h = static_cast<int>(app.entry_height * 0.55);
  int acc = 0;
  int prev_type = -1;
  for (int vi = 0; vi < static_cast<int>(app.cur_tab().visible_entries.size()); ++vi) {
    int r = app.cur_tab().visible_entries[vi];
    if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size())) {
      int t = static_cast<int>(app.cur_tab().entries[r].type);
      if (t != prev_type) { acc += hdr_h; prev_type = t; }
    }
    if (rel_y >= acc && rel_y < acc + app.entry_height) return vi;
    acc += app.entry_height;
  }
  return -1;
}

int hit_test_grid(AppState& app, int x, int y) {
  int sidebar_w = app.sidebar_expanded ? app.sidebar_width : 0;
  int content_x = sidebar_w;
  int content_y = app.top_bar_height + app.tab_bar_height;
  int content_w = app.width - sidebar_w;

  // Split pane adjustment
  if (app.split_view) {
    int div_w = 4;
    int split = app.split_divider_x;
    if (split <= 0) split = content_w / 2;
    if (app.active_pane == 1) {
      int right_x = std::min(content_x + content_w - 100, content_x + split + div_w / 2);
      content_w = content_x + content_w - right_x;
      content_x = right_x;
    } else {
      content_w = std::max(100, split - div_w / 2);
    }
  }

  int view_h = app.height - content_y - app.status_bar_height;

  if (x < content_x || x >= content_x + content_w) return -1;
  if (y < content_y || y >= content_y + view_h) return -1;

  double zf = app.zoom_pct / 100.0;
  int cell_size = app.grid_cell_size;
  int col_gap = app.grid_cell_gap;
  int row_gap = static_cast<int>(10.0 * zf);
  int cols = app.grid_cols;
  int row_h = app.grid_row_h;
  if (row_h <= 0) return -1;
  int item_h = row_h - row_gap;

  int grid_w = cols * cell_size + (cols - 1) * col_gap;
  int grid_offset_x = (content_w - grid_w) / 2;

  int rel_x = x - content_x - grid_offset_x;
  int rel_y = y - content_y - row_gap + app.cur_tab().scroll_px;

  int col = (rel_x + col_gap / 2) / (cell_size + col_gap);
  int row = (rel_y + row_gap / 2) / row_h;

  int idx = row * cols + col;
  if (idx < 0 || idx >= static_cast<int>(app.cur_tab().visible_entries.size()))
    return -1;

  int cx = col * (cell_size + col_gap);
  int cy = row * row_h;
  if (rel_x < cx || rel_x > cx + cell_size) return -1;
  if (rel_y < cy || rel_y > cy + item_h) return -1;

  return idx;
}

int hit_test_sidebar(AppState& app, int x, int y) {
  if (!app.sidebar_expanded) return -1;
  if (x < 0 || x >= app.sidebar_width) return -1;
  if (y < static_cast<int>(24.0 * 1.2) ||
      y >= app.height - app.status_bar_height)
    return -1;

  double zf = 1.2;
  int total = static_cast<int>(app.sidebar_locations.size());

  // Section boundaries (must match draw_sidebar)
  int places_end = 0;
  while (places_end < total &&
         app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Favorite &&
         app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Root &&
         app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Drive)
    ++places_end;

  int fav_start = places_end;
  while (fav_start < total &&
         app.sidebar_locations[fav_start].kind == SidebarLocation::Kind::Favorite)
    ++fav_start;

  int drives_start = fav_start;
  int fav_count = fav_start - places_end;
  int drive_count = total - drives_start;

  int padding = static_cast<int>(24.0 * zf);
  int header_h = static_cast<int>(24.0 * zf);
  int item_h = static_cast<int>(36.0 * zf);
  int div_pad = static_cast<int>(16.0 * zf);
  int div_total = div_pad + 1 + div_pad;

  int rel_y = y + app.sidebar_scroll_px;

  // ── PLACES header ──
  if (rel_y < padding + header_h) return -1;
  int pos = rel_y - padding - header_h;

  // ── Places items ──
  if (pos < places_end * item_h)
    return pos / item_h;

  if (places_end >= total) return -1;

  // ── Favorites section ──
  if (fav_count > 0) {
    pos -= places_end * item_h + div_total + header_h;

    // Favorites items
    if (pos >= 0 && pos < fav_count * item_h)
      return places_end + pos / item_h;

    // Skip divider + DRIVES header
    pos -= fav_count * item_h + div_total + header_h;
  } else {
    // No favorites — skip divider + DRIVES header
    pos -= places_end * item_h + div_total + header_h;
  }

  // ── Drives items (variable height: drives with usage data are taller) ──
  if (pos >= 0) {
    for (int i = drives_start; i < total; ++i) {
      const auto& loc = app.sidebar_locations[i];
      int dih = item_h;
      if ((loc.kind == SidebarLocation::Kind::Drive ||
           loc.kind == SidebarLocation::Kind::Root) &&
          loc.total_bytes > 0 && loc.is_mounted)
        dih += static_cast<int>(16.0 * zf);
      if (pos < dih) return i;
      pos -= dih;
    }
  }

  return -1;
}

bool hit_test_fav_section(AppState& app, int x, int y) {
  if (!app.sidebar_expanded) return false;
  if (x < 0 || x >= app.sidebar_width) return false;
  if (y < static_cast<int>(24.0 * 1.2) ||
      y >= app.height - app.status_bar_height)
    return false;

  double zf = 1.2;
  int total = static_cast<int>(app.sidebar_locations.size());

  int places_end = 0;
  while (places_end < total &&
         app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Favorite &&
         app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Root &&
         app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Drive)
    ++places_end;

  int fav_start = places_end;
  while (fav_start < total &&
         app.sidebar_locations[fav_start].kind == SidebarLocation::Kind::Favorite)
    ++fav_start;

  int drives_start = fav_start;
  int fav_count = fav_start - places_end;
  if (fav_count == 0 && drives_start >= total) return false;

  int padding = static_cast<int>(24.0 * zf);
  int header_h = static_cast<int>(24.0 * zf);
  int item_h = static_cast<int>(36.0 * zf);
  int div_pad = static_cast<int>(16.0 * zf);
  int div_total = div_pad + 1 + div_pad;

  int rel_y = y + app.sidebar_scroll_px;

  // Skip padding + PLACES header + items — Favorites section starts at the divider
  int places_bottom = padding + header_h + places_end * item_h;

  // Favorites items area: from the divider after Places to end of last fav item
  if (fav_count > 0) {
    int fav_end = places_bottom + div_total + header_h + fav_count * item_h;
    return rel_y >= places_bottom && rel_y < fav_end;
  }

  // No favorites — the "Add to Favorites" zone is the divider area after Places
  return rel_y >= places_bottom && rel_y < places_bottom + div_total;
}

int hit_test_context_menu(AppState& app, int x, int y) {
  if (!app.context_menu_open) return -1;
  int cm_x = app.context_menu_x;
  int cm_y = app.context_menu_y;
  int cm_w = 240;
  int cm_h = 0;
  for (const auto& item : app.context_menu_items) {
    cm_h += (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) ? 9 : 34;
  }

  // Clamp to screen (must match draw_context_menu)
  if (cm_x + cm_w > app.width - 8) cm_x = app.width - cm_w - 8;
  if (cm_y + cm_h > app.height - 8) cm_y = app.height - cm_h - 8;
  if (cm_x < 8) cm_x = 8;
  if (cm_y < 8) cm_y = 8;

  // Check submenu first if hovered item has one
  if (app.context_menu_hover >= 0 &&
      static_cast<size_t>(app.context_menu_hover) < app.context_menu_items.size() &&
      !app.context_menu_items[app.context_menu_hover].sub_items.empty()) {
    int sub_x = cm_x + cm_w + 3;
    int sub_y = cm_y;
    for (int j = 0; j < app.context_menu_hover; ++j) {
      sub_y += (app.context_menu_items[j].action == AppState::ContextMenuAction::Separator && app.context_menu_items[j].sub_items.empty()) ? 9 : 34;
    }
    int sm_w = 200;
    int sm_h = 0;
    const auto& subs = app.context_menu_items[app.context_menu_hover].sub_items;
    for (const auto& si : subs) {
      sm_h += (si.action == AppState::ContextMenuAction::Separator && si.sub_items.empty()) ? 9 : 34;
    }
    if (x >= sub_x && x < sub_x + sm_w && y >= sub_y && y < sub_y + sm_h) {
      // Hit on submenu - return index encoded as negative offset from -10
      int rel_y = y - sub_y;
      for (size_t i = 0; i < subs.size(); ++i) {
        int h = (subs[i].action == AppState::ContextMenuAction::Separator && subs[i].sub_items.empty()) ? 9 : 34;
        if (rel_y < h) {
          if (subs[i].action == AppState::ContextMenuAction::Separator) return -1;
          return -10 - static_cast<int>(i);
        }
        rel_y -= h;
      }
      return -1;
    }
  }

  // Check main menu
  if (x < cm_x || x >= cm_x + cm_w || y < cm_y || y >= cm_y + cm_h)
    return -1;
  int rel_y = y - cm_y;
  for (size_t i = 0; i < app.context_menu_items.size(); ++i) {
    int h = (app.context_menu_items[i].action == AppState::ContextMenuAction::Separator &&
             app.context_menu_items[i].sub_items.empty()) ? 9 : 34;
    if (rel_y < h) return static_cast<int>(i);
    rel_y -= h;
  }
  return -1;
}

// ── Open With dialog ──────────────────────────────────────────────

void draw_open_with(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  double sa = 1.0;

  // Dimmed backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  int total_entries = static_cast<int>(app.open_with_apps.size());
  if (total_entries == 0) return;

  int rec_count = app.open_with_exact_count;
  int other_count = total_entries - rec_count;

  int card_w = 420;
  int top_bar_h = 44;
  int entry_h = 40;
  int section_h = 26;
  int pad = 16;
  int pad_in = 12;
  int bottom_h = 52;
  int max_list_h = 320;

  // Compute total content height including section headers
  int total_content_h = total_entries * entry_h;
  if (rec_count > 0) total_content_h += section_h;
  if (other_count > 0) total_content_h += section_h;

  int list_h = std::min(total_content_h, max_list_h);

  // Clamp pixel scroll
  int max_scroll = std::max(0, total_content_h - list_h);
  app.open_with_scroll = std::clamp(app.open_with_scroll, 0, max_scroll);

  int card_h = pad + top_bar_h + pad_in + list_h + pad_in + bottom_h + pad;

  int cx = (w - card_w) / 2;
  int cy = (h - card_h) / 2;

  app.open_with_w = static_cast<double>(card_w);
  app.open_with_h = static_cast<double>(card_h);
  app.open_with_x = static_cast<double>(cx);
  app.open_with_y = static_cast<double>(cy);

  // Shadow
  cairo_set_source_rgba(cr, 0, 0, 0, 0.28);
  draw_rounded_rect(cr, cx + 2, cy + 3, card_w, card_h, 16);
  cairo_fill(cr);

  // Card background
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, sa);
  draw_rounded_rect(cr, cx, cy, card_w, card_h, 16);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  // Close button
  int close_sz = 28;
  app.open_with_hit_close[0] = cx + card_w - pad - close_sz;
  app.open_with_hit_close[1] = cy + pad - 4;
  app.open_with_hit_close[2] = close_sz;
  app.open_with_hit_close[3] = close_sz;
  {
    bool hov = (app.open_with_hover == -2);
    cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, hov ? 0.55 : 0.40);
    draw_rounded_rect(cr, app.open_with_hit_close[0], app.open_with_hit_close[1],
                       close_sz, close_sz, 8);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 18);
    cairo_move_to(cr, app.open_with_hit_close[0] + 7, app.open_with_hit_close[1] + 21);
    cairo_show_text(cr, "\u00D7");
  }

  // Title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 16);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, cx + pad, cy + pad + 18);
  cairo_show_text(cr, "Open With");

  // Filename
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                         app.text_secondary_b, 1.0);
  std::string fname = fs::path(app.open_with_file_path).filename().string();
  cairo_move_to(cr, cx + pad, cy + pad + 36);
  cairo_show_text(cr, fname.c_str());

  // Separator under top bar
  int sep1_y = cy + pad + top_bar_h;
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.15);
  cairo_rectangle(cr, cx, sep1_y, card_w, 1);
  cairo_fill(cr);

  // Clipped list area
  int list_x = cx + pad_in;
  int list_y = cy + pad + top_bar_h + pad_in;
  int list_w = card_w - 2 * pad_in;
  int scrollbar_w = 6;

  cairo_save(cr);
  cairo_rectangle(cr, list_x, list_y, list_w, list_h);
  cairo_clip(cr);

  int scroll_px = app.open_with_scroll;
  int cy_off = 0;

  for (int i = 0; i < total_entries; ++i) {
    // "Recommended" section header before first recommended item
    if (i == 0 && rec_count > 0) {
      if (cy_off + section_h > scroll_px && cy_off < scroll_px + list_h) {
        int sy = list_y + cy_off - scroll_px;
        // Header background
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.06);
        cairo_rectangle(cr, list_x, sy, list_w, section_h);
        cairo_fill(cr);
        // Label
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        cairo_move_to(cr, list_x + 8, sy + 17);
        cairo_show_text(cr, "Recommended");
      }
      cy_off += section_h;
    }

    // "Other Applications" section header before first non-recommended item
    if (i == rec_count && other_count > 0) {
      if (cy_off + section_h > scroll_px && cy_off < scroll_px + list_h) {
        int sy = list_y + cy_off - scroll_px;
        cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.06);
        cairo_rectangle(cr, list_x, sy, list_w, section_h);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                               app.text_secondary_b, 0.75);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        cairo_move_to(cr, list_x + 8, sy + 17);
        cairo_show_text(cr, "Other Applications");
      }
      cy_off += section_h;
    }

    // Item row
    if (cy_off + entry_h > scroll_px && cy_off < scroll_px + list_h) {
      int ey = list_y + cy_off - scroll_px;

      // Divider between entries
      if (cy_off > 0 || (i > 0)) {
        cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.08);
        cairo_rectangle(cr, list_x + 8, ey, list_w - 16, 1);
        cairo_fill(cr);
      }

      bool hov = (i == app.open_with_hover);
      if (hov) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.10);
        cairo_rectangle(cr, list_x, ey, list_w, entry_h);
        cairo_fill(cr);
      }

      // App icon
      int icon_size = 24;
      int icon_x = list_x + 6;
      int icon_y = ey + (entry_h - icon_size) / 2;
      const auto* icon_entry = app.icons.app_icon(app.open_with_apps[i].desktop_id);
      if (icon_entry && icon_entry->surface) {
        double iw = static_cast<double>(icon_entry->width);
        double ih = static_cast<double>(icon_entry->height);
        double scale = icon_size / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_translate(cr, icon_x, icon_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, icon_entry->surface,
                                 ((icon_size / scale) - iw) * 0.5,
                                 ((icon_size / scale) - ih) * 0.5);
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        // Fallback: colored circle + first letter
        cairo_arc(cr, icon_x + icon_size * 0.5, icon_y + icon_size * 0.5,
                  icon_size * 0.5, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
        cairo_fill(cr);
        char letter[2] = {app.open_with_apps[i].name.empty() ? '?' : app.open_with_apps[i].name[0], '\0'};
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t te;
        cairo_text_extents(cr, letter, &te);
        cairo_move_to(cr, icon_x + (icon_size - te.width) * 0.5 - te.x_bearing,
                      icon_y + (icon_size + te.height) * 0.5 - te.y_bearing);
        cairo_show_text(cr, letter);
      }

      // App name
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 13);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_move_to(cr, icon_x + icon_size + 8, ey + entry_h / 2 + 5);
      cairo_show_text(cr, app.open_with_apps[i].name.c_str());
    }

    cy_off += entry_h;
  }

  cairo_restore(cr);

  // Scrollbar
  if (total_content_h > list_h) {
    int sb_track_h = list_h;
    int sb_h = std::max(scrollbar_w * 2,
                        sb_track_h * list_h / total_content_h);
    int sb_max = sb_track_h - sb_h;
    double frac = max_scroll > 0
        ? static_cast<double>(scroll_px) / static_cast<double>(max_scroll)
        : 0.0;
    int sb_y = list_y + static_cast<int>(frac * sb_max);
    int sx = list_x + list_w - scrollbar_w - 2;
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, sx, sb_y, scrollbar_w, sb_h, scrollbar_w / 2);
    cairo_fill(cr);
  }

  // Separator above bottom bar
  int sep2_y = cy + pad + top_bar_h + pad_in + list_h + pad_in;
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.15);
  cairo_rectangle(cr, cx, sep2_y, card_w, 1);
  cairo_fill(cr);

  // Bottom bar buttons
  int bottom_y = sep2_y + 1;
  int btn_h = 34;
  int btn_w = 90;
  int btn_gap = 10;
  int btn_y = bottom_y + (bottom_h - btn_h) / 2;
  int btn_right = cx + card_w - pad;

  app.open_with_hit_cancel[0] = btn_right - btn_w * 2 - btn_gap;
  app.open_with_hit_cancel[1] = btn_y;
  app.open_with_hit_cancel[2] = btn_w;
  app.open_with_hit_cancel[3] = btn_h;

  app.open_with_hit_open[0] = btn_right - btn_w;
  app.open_with_hit_open[1] = btn_y;
  app.open_with_hit_open[2] = btn_w;
  app.open_with_hit_open[3] = btn_h;

  // Cancel button
  {
    bool hov = (app.open_with_hover == -3);
    cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, hov ? 0.52 : 0.42);
    draw_rounded_rect(cr, app.open_with_hit_cancel[0], btn_y, btn_w, btn_h, btn_h / 2);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.45);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_text_extents_t te;
    cairo_text_extents(cr, "Cancel", &te);
    cairo_move_to(cr, app.open_with_hit_cancel[0] + (btn_w - te.x_advance) / 2,
                  btn_y + btn_h / 2 + te.height * 0.35);
    cairo_show_text(cr, "Cancel");
  }

  // "Set as Default" toggle (left side of bottom bar)
  {
    int toggle_x = cx + pad;
    int toggle_y = btn_y;
    int toggle_h = btn_h;
    bool hov = (app.open_with_hover == -5);
    bool on = app.open_with_set_default;

    app.open_with_hit_default[0] = toggle_x;
    app.open_with_hit_default[1] = toggle_y;
    app.open_with_hit_default[2] = 160;
    app.open_with_hit_default[3] = toggle_h;

    // Toggle track
    int track_w = 40;
    int track_h = 20;
    int track_x = toggle_x;
    int track_y = toggle_y + (toggle_h - track_h) / 2;
    double radius = track_h / 2.0;
    if (on) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
    } else {
      cairo_set_source_rgba(cr, 0.45, 0.45, 0.45, hov ? 0.9 : 0.7);
    }
    draw_rounded_rect(cr, track_x, track_y, track_w, track_h, radius);
    cairo_fill(cr);

    // Toggle knob
    int knob_sz = 16;
    double knob_cx = on ? track_x + track_w - track_h / 2.0 : track_x + track_h / 2.0;
    double knob_cy = track_y + track_h / 2.0;
    cairo_arc(cr, knob_cx, knob_cy, knob_sz / 2.0, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_fill(cr);

    // Label
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, hov ? 1.0 : 0.9);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, track_x + track_w + 10, toggle_y + toggle_h / 2 + 5);
    cairo_show_text(cr, "Set as default");
  }

  // Open button
  {
    bool hov = (app.open_with_hover == -4);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, hov ? 0.22 : 0.12);
    draw_rounded_rect(cr, app.open_with_hit_open[0], btn_y, btn_w, btn_h, btn_h / 2);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_text_extents_t te;
    cairo_text_extents(cr, "Open", &te);
    cairo_move_to(cr, app.open_with_hit_open[0] + (btn_w - te.x_advance) / 2,
                  btn_y + btn_h / 2 + te.height * 0.35);
    cairo_show_text(cr, "Open");
  }
}

void draw_settings_dialog(AppState& app, cairo_t* cr) {
  int card_w = 420;
  int card_h = 560;
  int cx = (app.width - card_w) / 2;
  int cy = (app.height - card_h) / 2;
  int pad = 20;
  int top_bar_h = 44;
  int tab_h = 36;

  app.settings_x = cx;
  app.settings_y = cy;
  app.settings_w = card_w;
  app.settings_h = card_h;

  // Card shadow
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  draw_rounded_rect(cr, cx + 2, cy + 4, card_w, card_h, 12);
  cairo_fill(cr);

  // Card background
  double dlg_bg_alpha = app.dialog_opacity_pct / 100.0;
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, dlg_bg_alpha);
  draw_rounded_rect(cr, cx, cy, card_w, card_h, 12);
  cairo_fill(cr);

  // Title bar
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 16);
  cairo_move_to(cr, cx + pad, cy + 24);
  cairo_show_text(cr, "File Browser Settings");

  // Close X
  double close_x = cx + card_w - pad - 24;
  double close_y = cy + 8;
  bool close_hov = (app.pointerX >= close_x && app.pointerX < close_x + 24 &&
                    app.pointerY >= close_y && app.pointerY < close_y + 24);
  if (close_hov) {
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.15);
    cairo_arc(cr, close_x + 12, close_y + 12, 12, 0, 2 * M_PI);
    cairo_fill(cr);
  }
  // Draw X
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.6);
  cairo_set_line_width(cr, 2);
  cairo_move_to(cr, close_x + 6, close_y + 6);
  cairo_line_to(cr, close_x + 18, close_y + 18);
  cairo_stroke(cr);
  cairo_move_to(cr, close_x + 18, close_y + 6);
  cairo_line_to(cr, close_x + 6, close_y + 18);
  cairo_stroke(cr);

  // Tabs
  int tab_y = cy + top_bar_h + 4;
  int tab_w = (card_w - 2 * pad) / 2;
  const char* tab_names[] = {"General", "Appearance"};
  for (int t = 0; t < 2; ++t) {
    int tx = cx + pad + t * tab_w;
    bool active = (t == app.settings_tab);
    bool tab_hov = (app.pointerX >= tx && app.pointerX < tx + tab_w &&
                    app.pointerY >= tab_y && app.pointerY < tab_y + tab_h);
    app.settings_tab_hit[t][0] = tx;
    app.settings_tab_hit[t][1] = tab_y;
    app.settings_tab_hit[t][2] = tab_w;
    app.settings_tab_hit[t][3] = tab_h;

    if (active) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.15);
    } else if (tab_hov) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.06);
    } else {
      cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.15);
    }
    draw_rounded_rect(cr, tx, tab_y, tab_w, tab_h, 6);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, active ? 1.0 : 0.6);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_text_extents_t te;
    cairo_text_extents(cr, tab_names[t], &te);
    cairo_move_to(cr, tx + (tab_w - te.x_advance) / 2, tab_y + tab_h / 2 + te.height * 0.35);
    cairo_show_text(cr, tab_names[t]);
  }

  int content_y = tab_y + tab_h + 12;

  // ── General tab ──
  if (app.settings_tab == 0) {
    int ly = content_y;
    int left_x = cx + pad + 8;

    // Zoom label
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Zoom");

    // Zoom value (editable inline)
    char zoom_str[16];
    if (app.settings_zoom_editing) {
      snprintf(zoom_str, sizeof(zoom_str), "%s|", app.settings_zoom_buf.c_str());
    } else {
      snprintf(zoom_str, sizeof(zoom_str), "%.0f%%", app.settings_zoom_pct);
    }
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_move_to(cr, left_x + 180, ly + 14);
    cairo_show_text(cr, zoom_str);

    // Zoom - button
    double z_btn_x = left_x + 220;
    double z_btn_y = ly - 4;
    double z_btn_s = 28;
    bool z_dec_hov = (app.pointerX >= z_btn_x && app.pointerX < z_btn_x + z_btn_s &&
                      app.pointerY >= z_btn_y && app.pointerY < z_btn_y + z_btn_s);
    app.settings_hit_zoom_down[0] = z_btn_x;
    app.settings_hit_zoom_down[1] = z_btn_y;
    app.settings_hit_zoom_down[2] = z_btn_s;
    app.settings_hit_zoom_down[3] = z_btn_s;

    cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, z_dec_hov ? 0.7 : 0.5);
    draw_rounded_rect(cr, z_btn_x, z_btn_y, z_btn_s, z_btn_s, 6);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.8);
    cairo_set_font_size(cr, 18);
    cairo_move_to(cr, z_btn_x + 8, z_btn_y + 20);
    cairo_show_text(cr, "-");

    // Zoom + button
    z_btn_x += z_btn_s + 6;
    bool z_inc_hov = (app.pointerX >= z_btn_x && app.pointerX < z_btn_x + z_btn_s &&
                      app.pointerY >= z_btn_y && app.pointerY < z_btn_y + z_btn_s);
    app.settings_hit_zoom_up[0] = z_btn_x;
    app.settings_hit_zoom_up[1] = z_btn_y;
    app.settings_hit_zoom_up[2] = z_btn_s;
    app.settings_hit_zoom_up[3] = z_btn_s;

    cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, z_inc_hov ? 0.7 : 0.5);
    draw_rounded_rect(cr, z_btn_x, z_btn_y, z_btn_s, z_btn_s, 6);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.8);
    cairo_set_font_size(cr, 18);
    cairo_move_to(cr, z_btn_x + 7, z_btn_y + 20);
    cairo_show_text(cr, "+");

    ly += 40;

    // Folders before files toggle
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Folders before files");

    double toggle_x = left_x + 220;
    double toggle_y = ly - 2;
    double toggle_w = 40;
    double toggle_h = 22;
    app.settings_hit_folders_toggle[0] = toggle_x;
    app.settings_hit_folders_toggle[1] = toggle_y;
    app.settings_hit_folders_toggle[2] = toggle_w;
    app.settings_hit_folders_toggle[3] = toggle_h;

    bool toggle_hov = (app.pointerX >= toggle_x && app.pointerX < toggle_x + toggle_w &&
                       app.pointerY >= toggle_y && app.pointerY < toggle_y + toggle_h);
    if (toggle_hov) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.1);
      draw_rounded_rect(cr, toggle_x - 2, toggle_y - 2, toggle_w + 4, toggle_h + 4, toggle_h / 2 + 2);
      cairo_fill(cr);
    }

    // Toggle track
    cairo_set_source_rgba(cr, app.settings_folders_before_files ? app.accent_r : app.outline_r,
                          app.settings_folders_before_files ? app.accent_g : app.outline_g,
                          app.settings_folders_before_files ? app.accent_b : app.outline_b,
                          0.6);
    draw_rounded_rect(cr, toggle_x, toggle_y, toggle_w, toggle_h, toggle_h / 2);
    cairo_fill(cr);

    // Toggle knob
    double knob_x = app.settings_folders_before_files ? toggle_x + toggle_w - toggle_h : toggle_x;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, knob_x + toggle_h / 2, toggle_y + toggle_h / 2, toggle_h / 2 - 2, 0, 2 * M_PI);
    cairo_fill(cr);

    ly += 40;

    // Default terminal dropdown
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Default terminal");

    double drop_x = left_x + 130;
    double drop_y = ly - 4;
    double drop_w = card_w - pad - 16 - drop_x + cx;
    double drop_h = 30;
    app.settings_hit_term_dropdown[0] = drop_x;
    app.settings_hit_term_dropdown[1] = drop_y;
    app.settings_hit_term_dropdown[2] = drop_w;
    app.settings_hit_term_dropdown[3] = drop_h;

    bool drop_hov = (app.pointerX >= drop_x && app.pointerX < drop_x + drop_w &&
                     app.pointerY >= drop_y && app.pointerY < drop_y + drop_h);

    // Dropdown box
    cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, drop_hov ? 0.7 : 0.5);
    draw_rounded_rect(cr, drop_x, drop_y, drop_w, drop_h, 6);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
    cairo_set_line_width(cr, 1);
    draw_rounded_rect(cr, drop_x, drop_y, drop_w, drop_h, 6);
    cairo_stroke(cr);

    // Selected item text
    std::string sel_label = "System default";
    if (app.settings_default_term_idx > 0 &&
        app.settings_default_term_idx - 1 < static_cast<int>(app.settings_term_opts.size())) {
      sel_label = app.settings_term_opts[app.settings_default_term_idx];
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, drop_x + 8, drop_y + drop_h / 2 + 5);
    cairo_show_text(cr, sel_label.c_str());

    // Dropdown arrow
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.6);
    cairo_move_to(cr, drop_x + drop_w - 16, drop_y + 10);
    cairo_line_to(cr, drop_x + drop_w - 10, drop_y + 20);
    cairo_line_to(cr, drop_x + drop_w - 4, drop_y + 10);
    cairo_stroke(cr);

    // Dropdown open: draw list below
    if (app.settings_dropdown_open) {
      int dd_entry_h = 28;
      int dd_max_visible = 6;
      int dd_total = static_cast<int>(app.settings_term_opts.size());
      int dd_visible = std::min(dd_total, dd_max_visible);
      int dd_list_h = dd_visible * dd_entry_h;
      int dd_y = static_cast<int>(drop_y + drop_h + 2);
      int dd_x = static_cast<int>(drop_x);

      // Dropdown list background
      cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.95);
      draw_rounded_rect(cr, dd_x, dd_y, drop_w, dd_list_h, 6);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
      cairo_set_line_width(cr, 1);
      draw_rounded_rect(cr, dd_x, dd_y, drop_w, dd_list_h, 6);
      cairo_stroke(cr);

      cairo_save(cr);
      cairo_rectangle(cr, dd_x, dd_y, drop_w, dd_list_h);
      cairo_clip(cr);

      int scroll_offset = app.settings_dropdown_scroll;
      for (int i = scroll_offset; i < dd_total && i < scroll_offset + dd_visible; ++i) {
        int item_y = dd_y + (i - scroll_offset) * dd_entry_h;
        bool item_hov = (i == app.settings_dropdown_hover);
        bool item_sel = (i == app.settings_default_term_idx);

        if (item_hov || item_sel) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                                item_hov ? 0.2 : 0.1);
          cairo_rectangle(cr, dd_x, item_y, drop_w, dd_entry_h);
          cairo_fill(cr);
        }

        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, item_sel ? 1.0 : 0.8);
        cairo_set_font_size(cr, 12);
        cairo_move_to(cr, dd_x + 8, item_y + dd_entry_h / 2 + 5);
        cairo_show_text(cr, app.settings_term_opts[i].c_str());
      }

      cairo_restore(cr);
    }
  }

  // ── Appearance tab ──
  if (app.settings_tab == 1) {
    int ly = content_y;
    int left_x = cx + pad + 8;

    // Surface opacity label
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Surface opacity");

    // Opacity value
    char op_str[16];
    snprintf(op_str, sizeof(op_str), "%d%%", app.settings_opacity_pct);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_move_to(cr, left_x + 180, ly + 14);
    cairo_show_text(cr, op_str);

    // Slider track
    int slider_x = cx + pad + 8;
    int slider_y = ly + 24;
    int slider_w = card_w - 2 * pad - 16;
    int slider_h = 6;
    app.settings_hit_opacity_slider[0] = slider_x;
    app.settings_hit_opacity_slider[1] = slider_y - 10;
    app.settings_hit_opacity_slider[2] = slider_w;
    app.settings_hit_opacity_slider[3] = slider_h + 20;

    // Track background
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, slider_x, slider_y, slider_w, slider_h, 3);
    cairo_fill(cr);

    // Filled portion
    double fill_w = slider_w * (app.settings_opacity_pct / 100.0);
    if (fill_w > 0) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
      draw_rounded_rect(cr, slider_x, slider_y, fill_w, slider_h, 3);
      cairo_fill(cr);
    }

    // Knob
    double knob_x = slider_x + fill_w;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, knob_x, slider_y + slider_h / 2, 8, 0, 2 * M_PI);
    cairo_fill(cr);

    // Sidebar opacity
    ly += 52;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Sidebar opacity");

    char sb_op_str[16];
    snprintf(sb_op_str, sizeof(sb_op_str), "%d%%", app.settings_sidebar_opacity_pct);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_move_to(cr, left_x + 180, ly + 14);
    cairo_show_text(cr, sb_op_str);

    int sb_slider_y = ly + 24;
    app.settings_hit_sidebar_opacity_slider[0] = slider_x;
    app.settings_hit_sidebar_opacity_slider[1] = sb_slider_y - 10;
    app.settings_hit_sidebar_opacity_slider[2] = slider_w;
    app.settings_hit_sidebar_opacity_slider[3] = slider_h + 20;

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, slider_x, sb_slider_y, slider_w, slider_h, 3);
    cairo_fill(cr);

    double sb_fill_w = slider_w * (app.settings_sidebar_opacity_pct / 100.0);
    if (sb_fill_w > 0) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
      draw_rounded_rect(cr, slider_x, sb_slider_y, sb_fill_w, slider_h, 3);
      cairo_fill(cr);
    }

    double sb_knob_x = slider_x + sb_fill_w;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, sb_knob_x, sb_slider_y + slider_h / 2, 8, 0, 2 * M_PI);
    cairo_fill(cr);

    // Top bar opacity
    ly += 52;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Top bar opacity");

    char tb_op_str[16];
    snprintf(tb_op_str, sizeof(tb_op_str), "%d%%", app.settings_topbar_opacity_pct);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_move_to(cr, left_x + 180, ly + 14);
    cairo_show_text(cr, tb_op_str);

    int tb_slider_y = ly + 24;
    app.settings_hit_topbar_opacity_slider[0] = slider_x;
    app.settings_hit_topbar_opacity_slider[1] = tb_slider_y - 10;
    app.settings_hit_topbar_opacity_slider[2] = slider_w;
    app.settings_hit_topbar_opacity_slider[3] = slider_h + 20;

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, slider_x, tb_slider_y, slider_w, slider_h, 3);
    cairo_fill(cr);

    double tb_fill_w = slider_w * (app.settings_topbar_opacity_pct / 100.0);
    if (tb_fill_w > 0) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
      draw_rounded_rect(cr, slider_x, tb_slider_y, tb_fill_w, slider_h, 3);
      cairo_fill(cr);
    }

    double tb_knob_x = slider_x + tb_fill_w;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, tb_knob_x, tb_slider_y + slider_h / 2, 8, 0, 2 * M_PI);
    cairo_fill(cr);

    // Status bar opacity
    ly += 52;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Status bar opacity");

    char st_op_str[16];
    snprintf(st_op_str, sizeof(st_op_str), "%d%%", app.settings_statusbar_opacity_pct);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_move_to(cr, left_x + 180, ly + 14);
    cairo_show_text(cr, st_op_str);

    int st_slider_y = ly + 24;
    app.settings_hit_statusbar_opacity_slider[0] = slider_x;
    app.settings_hit_statusbar_opacity_slider[1] = st_slider_y - 10;
    app.settings_hit_statusbar_opacity_slider[2] = slider_w;
    app.settings_hit_statusbar_opacity_slider[3] = slider_h + 20;

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, slider_x, st_slider_y, slider_w, slider_h, 3);
    cairo_fill(cr);

    double st_fill_w = slider_w * (app.settings_statusbar_opacity_pct / 100.0);
    if (st_fill_w > 0) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
      draw_rounded_rect(cr, slider_x, st_slider_y, st_fill_w, slider_h, 3);
      cairo_fill(cr);
    }

    double st_knob_x = slider_x + st_fill_w;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, st_knob_x, st_slider_y + slider_h / 2, 8, 0, 2 * M_PI);
    cairo_fill(cr);

    // Preview opacity
    ly += 52;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Preview opacity");

    char pv_op_str[16];
    snprintf(pv_op_str, sizeof(pv_op_str), "%d%%", app.settings_preview_opacity_pct);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_move_to(cr, left_x + 180, ly + 14);
    cairo_show_text(cr, pv_op_str);

    int pv_slider_y = ly + 24;
    app.settings_hit_preview_opacity_slider[0] = slider_x;
    app.settings_hit_preview_opacity_slider[1] = pv_slider_y - 10;
    app.settings_hit_preview_opacity_slider[2] = slider_w;
    app.settings_hit_preview_opacity_slider[3] = slider_h + 20;

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, slider_x, pv_slider_y, slider_w, slider_h, 3);
    cairo_fill(cr);

    double pv_fill_w = slider_w * (app.settings_preview_opacity_pct / 100.0);
    if (pv_fill_w > 0) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
      draw_rounded_rect(cr, slider_x, pv_slider_y, pv_fill_w, slider_h, 3);
      cairo_fill(cr);
    }

    double pv_knob_x = slider_x + pv_fill_w;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, pv_knob_x, pv_slider_y + slider_h / 2, 8, 0, 2 * M_PI);
    cairo_fill(cr);

    // Settings dialog opacity
    ly += 52;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Settings dialog opacity");

    char dlg_op_str[16];
    snprintf(dlg_op_str, sizeof(dlg_op_str), "%d%%", app.settings_dialog_opacity_pct);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_move_to(cr, left_x + 200, ly + 14);
    cairo_show_text(cr, dlg_op_str);

    int dlg_slider_y = ly + 24;
    app.settings_hit_dialog_opacity_slider[0] = slider_x;
    app.settings_hit_dialog_opacity_slider[1] = dlg_slider_y - 10;
    app.settings_hit_dialog_opacity_slider[2] = slider_w;
    app.settings_hit_dialog_opacity_slider[3] = slider_h + 20;

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, slider_x, dlg_slider_y, slider_w, slider_h, 3);
    cairo_fill(cr);

    double dlg_fill_w = slider_w * (app.settings_dialog_opacity_pct / 100.0);
    if (dlg_fill_w > 0) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
      draw_rounded_rect(cr, slider_x, dlg_slider_y, dlg_fill_w, slider_h, 3);
      cairo_fill(cr);
    }

    double dlg_knob_x = slider_x + dlg_fill_w;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, dlg_knob_x, dlg_slider_y + slider_h / 2, 8, 0, 2 * M_PI);
    cairo_fill(cr);

    // Properties dialog opacity
    ly += 52;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Properties dialog opacity");

    char prp_op_str[16];
    snprintf(prp_op_str, sizeof(prp_op_str), "%d%%", app.settings_properties_opacity_pct);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_move_to(cr, left_x + 200, ly + 14);
    cairo_show_text(cr, prp_op_str);

    int prp_slider_y = ly + 24;
    app.settings_hit_properties_opacity_slider[0] = slider_x;
    app.settings_hit_properties_opacity_slider[1] = prp_slider_y - 10;
    app.settings_hit_properties_opacity_slider[2] = slider_w;
    app.settings_hit_properties_opacity_slider[3] = slider_h + 20;

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, slider_x, prp_slider_y, slider_w, slider_h, 3);
    cairo_fill(cr);

    double prp_fill_w = slider_w * (app.settings_properties_opacity_pct / 100.0);
    if (prp_fill_w > 0) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
      draw_rounded_rect(cr, slider_x, prp_slider_y, prp_fill_w, slider_h, 3);
      cairo_fill(cr);
    }

    double prp_knob_x = slider_x + prp_fill_w;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, prp_knob_x, prp_slider_y + slider_h / 2, 8, 0, 2 * M_PI);
    cairo_fill(cr);

    // Matugen wallpaper theming toggle
    ly += 52;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Matugen wallpaper colors");

    double mt_toggle_x = left_x + 220;
    double mt_toggle_y = ly - 2;
    double mt_toggle_w = 40;
    double mt_toggle_h = 22;
    app.settings_hit_matugen_toggle[0] = mt_toggle_x;
    app.settings_hit_matugen_toggle[1] = mt_toggle_y;
    app.settings_hit_matugen_toggle[2] = mt_toggle_w;
    app.settings_hit_matugen_toggle[3] = mt_toggle_h;

    bool mt_hov = (app.pointerX >= mt_toggle_x && app.pointerX < mt_toggle_x + mt_toggle_w &&
                   app.pointerY >= mt_toggle_y && app.pointerY < mt_toggle_y + mt_toggle_h);
    if (mt_hov) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.1);
      draw_rounded_rect(cr, mt_toggle_x - 2, mt_toggle_y - 2, mt_toggle_w + 4, mt_toggle_h + 4, mt_toggle_h / 2 + 2);
      cairo_fill(cr);
    }

    cairo_set_source_rgba(cr, app.settings_matugen_theming ? app.accent_r : app.outline_r,
                          app.settings_matugen_theming ? app.accent_g : app.outline_g,
                          app.settings_matugen_theming ? app.accent_b : app.outline_b,
                          0.6);
    draw_rounded_rect(cr, mt_toggle_x, mt_toggle_y, mt_toggle_w, mt_toggle_h, mt_toggle_h / 2);
    cairo_fill(cr);

    double mt_knob_x = app.settings_matugen_theming ? mt_toggle_x + mt_toggle_w - mt_toggle_h : mt_toggle_x;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, mt_knob_x + mt_toggle_h / 2, mt_toggle_y + mt_toggle_h / 2, mt_toggle_h / 2 - 2, 0, 2 * M_PI);
    cairo_fill(cr);
  }

  // Bottom buttons
  int btn_w = 80;
  int btn_h = 30;
  int btn_gap = 10;
  int btn_y = cy + card_h - 50;

  // Cancel
  app.settings_hit_cancel[0] = cx + card_w - pad - btn_w * 2 - btn_gap;
  app.settings_hit_cancel[1] = btn_y;
  app.settings_hit_cancel[2] = btn_w;
  app.settings_hit_cancel[3] = btn_h;

  bool cancel_hov = (app.pointerX >= app.settings_hit_cancel[0] && app.pointerX < app.settings_hit_cancel[0] + btn_w &&
                     app.pointerY >= btn_y && app.pointerY < btn_y + btn_h);
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, cancel_hov ? 0.8 : 0.55);
  draw_rounded_rect(cr, app.settings_hit_cancel[0], btn_y, btn_w, btn_h, btn_h / 2);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.45);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_text_extents_t te;
  cairo_text_extents(cr, "Cancel", &te);
  cairo_move_to(cr, app.settings_hit_cancel[0] + (btn_w - te.x_advance) / 2,
                btn_y + btn_h / 2 + te.height * 0.35);
  cairo_show_text(cr, "Cancel");

  // Apply
  app.settings_hit_apply[0] = cx + card_w - pad - btn_w;
  app.settings_hit_apply[1] = btn_y;
  app.settings_hit_apply[2] = btn_w;
  app.settings_hit_apply[3] = btn_h;

  bool apply_hov = (app.pointerX >= app.settings_hit_apply[0] && app.pointerX < app.settings_hit_apply[0] + btn_w &&
                    app.pointerY >= btn_y && app.pointerY < btn_y + btn_h);
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, apply_hov ? 0.8 : 0.55);
  draw_rounded_rect(cr, app.settings_hit_apply[0], btn_y, btn_w, btn_h, btn_h / 2);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.45);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_set_font_size(cr, 13);
  cairo_text_extents(cr, "Apply", &te);
  cairo_move_to(cr, app.settings_hit_apply[0] + (btn_w - te.x_advance) / 2,
                btn_y + btn_h / 2 + te.height * 0.35);
  cairo_show_text(cr, "Apply");

  // OK
  app.settings_hit_ok[0] = cx + card_w - pad - btn_w * 3 - btn_gap * 2;
  app.settings_hit_ok[1] = btn_y;
  app.settings_hit_ok[2] = btn_w;
  app.settings_hit_ok[3] = btn_h;

  bool ok_hov = (app.pointerX >= app.settings_hit_ok[0] && app.pointerX < app.settings_hit_ok[0] + btn_w &&
                 app.pointerY >= btn_y && app.pointerY < btn_y + btn_h);
  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, ok_hov ? 0.25 : 0.12);
  draw_rounded_rect(cr, app.settings_hit_ok[0], btn_y, btn_w, btn_h, btn_h / 2);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_set_font_size(cr, 13);
  cairo_text_extents(cr, "OK", &te);
  cairo_move_to(cr, app.settings_hit_ok[0] + (btn_w - te.x_advance) / 2,
                btn_y + btn_h / 2 + te.height * 0.35);
  cairo_show_text(cr, "OK");
}

static void draw_separator(cairo_t* cr, int x, int y, int w) {
  cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.12);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, x, y);
  cairo_line_to(cr, x + w, y);
  cairo_stroke(cr);
}

void draw_properties_dialog(AppState& app, cairo_t* cr) {
  auto& p = app.properties;
  if (!p.open) return;

  const int card_w = 520;
  const int card_h = 560;
  const int cx = (app.width - card_w) / 2;
  const int cy = (app.height - card_h) / 2;
  const int pad = 24;
  const int icon_size = 48;

  p.x = cx; p.y = cy; p.w = card_w; p.h = card_h;

  // ── Shadow (soft multi-layer) ──
  for (int s = 3; s >= 0; --s) {
    cairo_set_source_rgba(cr, 0, 0, 0, 0.05 * (4 - s));
    draw_rounded_rect(cr, cx + s * 1.5, cy + s * 2.5, card_w, card_h, 16);
    cairo_fill(cr);
  }

  // ── Card background ──
  double prp_bg_alpha = app.properties_opacity_pct / 100.0;
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, prp_bg_alpha);
  draw_rounded_rect(cr, cx, cy, card_w, card_h, 16);
  cairo_fill(cr);

  // ── Header: centered icon + centered name + centered type ──
  auto icon = app.icons.tray_icon(p.multi ? "folder-multiple"
           : (p.icon_name.empty() ? (p.is_dir ? "folder" : "text-x-generic") : p.icon_name));
  if (!icon && p.multi) icon = app.icons.tray_icon("text-x-generic");
  if (icon && icon->surface) {
    double iw = icon->width, ih = icon->height;
    if (iw > 0 && ih > 0) {
      double scale = icon_size / std::max(1.0, std::max(iw, ih));
      cairo_save(cr);
      cairo_translate(cr, cx + (card_w - icon_size) / 2, cy + pad);
      cairo_scale(cr, scale, scale);
      cairo_set_source_surface(cr, icon->surface,
                               (icon_size / scale - iw) / 2,
                               (icon_size / scale - ih) / 2);
      cairo_paint(cr);
      cairo_restore(cr);
    }
  }

  // Filename (centered, bold)
  int name_y = cy + pad + icon_size + 14;
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  std::string disp_name = p.name;
  cairo_text_extents_t te;
  cairo_text_extents(cr, disp_name.c_str(), &te);
  int name_max_w = card_w - 2 * pad;
  if (te.x_advance > name_max_w) {
    while (!disp_name.empty() && te.x_advance > name_max_w) {
      disp_name.pop_back();
      cairo_text_extents(cr, (disp_name + "\u2026").c_str(), &te);
    }
    disp_name = disp_name.empty() ? "\u2026" : disp_name + "\u2026";
  }
  cairo_move_to(cr, cx + (card_w - te.x_advance) / 2, name_y);
  cairo_show_text(cr, disp_name.c_str());

  // Type line (centered, secondary) — mime type, or selection summary for multi
  int type_y = name_y + 20;
  std::string type_line = p.multi ? std::string("Multiple selection") : p.mime_type;
  if (!type_line.empty()) {
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.55);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_text_extents(cr, type_line.c_str(), &te);
    cairo_move_to(cr, cx + (card_w - te.x_advance) / 2, type_y);
    cairo_show_text(cr, type_line.c_str());
  }

  // Close X (top right)
  double close_x = cx + card_w - pad - 26;
  double close_y = cy + pad - 2;
  bool close_hov = (app.pointerX >= close_x && app.pointerX < close_x + 24 &&
                    app.pointerY >= close_y && app.pointerY < close_y + 24);
  p.hit_close[0] = close_x; p.hit_close[1] = close_y;
  p.hit_close[2] = 24; p.hit_close[3] = 24;
  if (close_hov) {
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
    draw_rounded_rect(cr, close_x, close_y, 24, 24, 12);
    cairo_fill(cr);
  }
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.4);
  cairo_set_line_width(cr, 1.5);
  cairo_move_to(cr, close_x + 7, close_y + 7);
  cairo_line_to(cr, close_x + 17, close_y + 17);
  cairo_stroke(cr);
  cairo_move_to(cr, close_x + 17, close_y + 7);
  cairo_line_to(cr, close_x + 7, close_y + 17);
  cairo_stroke(cr);

  // Header separator
  int header_sep_y = type_y + (type_line.empty() ? 12 : 16);
  draw_separator(cr, cx + pad, header_sep_y, card_w - 2 * pad);

  // ── Tabs ──
  int tab_y = header_sep_y + 10;
  int tab_h = 32;
  int content_of_tab[4];
  int num_tabs = 0;
  content_of_tab[num_tabs++] = 0;
  content_of_tab[num_tabs++] = 1;
  bool has_image = (p.image_w > 0 && p.image_h > 0);
  bool has_media = p.is_media;
  if (has_image) content_of_tab[num_tabs++] = 2;
  if (has_media)  content_of_tab[num_tabs++] = 3;
  const char* tab_labels[4] = {"Basic", "Permissions", "Image", "Media"};
  int tab_gap = 4;
  int tab_w = (card_w - 2 * pad - tab_gap * (num_tabs - 1)) / num_tabs;

  for (int t = 0; t < num_tabs; ++t) {
    int ct = content_of_tab[t];
    int tx = cx + pad + t * (tab_w + tab_gap);
    bool active = (t == p.tab);
    bool tab_hov = (app.pointerX >= tx && app.pointerX < tx + tab_w &&
                    app.pointerY >= tab_y && app.pointerY < tab_y + tab_h);
    p.hit_tabs[t][0] = tx; p.hit_tabs[t][1] = tab_y;
    p.hit_tabs[t][2] = tab_w; p.hit_tabs[t][3] = tab_h;

    if (tab_hov && !active) {
      cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.06);
      draw_rounded_rect(cr, tx + 2, tab_y, tab_w - 4, tab_h, 6);
      cairo_fill(cr);
    }

    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, active ? 0.95 : 0.45);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_text_extents_t te;
    cairo_text_extents(cr, tab_labels[ct], &te);
    cairo_move_to(cr, tx + (tab_w - te.x_advance) / 2, tab_y + tab_h / 2 + te.height * 0.35);
    cairo_show_text(cr, tab_labels[ct]);

    if (active) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.9);
      draw_rounded_rect(cr, tx + 12, tab_y + tab_h - 3, tab_w - 24, 2, 1);
      cairo_fill(cr);
    }
  }

  int content_tab = (p.tab >= 0 && p.tab < num_tabs) ? content_of_tab[p.tab] : 0;

  // ── Content area ──
  int content_y0 = tab_y + tab_h + 8;
  int content_h_max = card_h - (content_y0 - cy) - 52;
  cairo_save(cr);
  cairo_rectangle(cr, cx + pad - 14, content_y0, card_w - 2 * pad + 28, content_h_max);
  cairo_clip(cr);

  int row_w = card_w - 2 * pad;
  int col1_x = cx + pad;
  int col2_x = cx + card_w - pad;
  int ly = content_y0 + 4 - p.scroll_px;

  // Helper: info row with right-aligned value
  auto draw_info_row = [&](const char* label, const std::string& value) {
    // Pill background — lighter than card, matugen-aware
    double pill_r = (app.surface_r + app.bg_r) * 0.5 + 0.12;
    double pill_g = (app.surface_g + app.bg_g) * 0.5 + 0.12;
    double pill_b = (app.surface_b + app.bg_b) * 0.5 + 0.12;
    cairo_set_source_rgba(cr, pill_r, pill_g, pill_b, 0.75);
    draw_rounded_rect(cr, col1_x - 14, ly + 2, row_w + 28, 28, 14);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_move_to(cr, col1_x + 2, ly + 18);
    cairo_show_text(cr, label);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_text_extents_t ve;
    cairo_text_extents(cr, value.c_str(), &ve);
    double vx = col2_x - ve.x_advance + 2;
    if (vx < col1_x + 122) vx = col1_x + 122;
    cairo_move_to(cr, vx, ly + 18);
    cairo_show_text(cr, value.c_str());
    ly += 32;
  };

  // Helper: section header with separator
  auto draw_section = [&](const char* title) {
    ly += 4;
    draw_separator(cr, col1_x, ly, row_w);
    ly += 14;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    cairo_move_to(cr, col1_x, ly + 14);
    cairo_show_text(cr, title);
    ly += 22;
  };

  // ── Basic tab ──
  if (content_tab == 0) {
  if (p.multi) {
    // Combined summary for a multi-selection
    std::string items_str;
    if (p.dir_count > 0 && p.file_count > 0)
      items_str = std::to_string(p.dir_count) + (p.dir_count == 1 ? " folder, " : " folders, ") +
                  std::to_string(p.file_count) + (p.file_count == 1 ? " file" : " files");
    else if (p.dir_count > 0)
      items_str = std::to_string(p.dir_count) + (p.dir_count == 1 ? " folder" : " folders");
    else
      items_str = std::to_string(p.file_count) + (p.file_count == 1 ? " file" : " files");
    draw_info_row("Items", items_str);

    char sz[64];
    double sz_val = static_cast<double>(p.size);
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int ui = 0;
    while (sz_val >= 1024.0 && ui < 4) { sz_val /= 1024.0; ++ui; }
    if (ui == 0)
      snprintf(sz, sizeof(sz), "%llu B", (unsigned long long)p.size);
    else
      snprintf(sz, sizeof(sz), "%.1f %s (%llu bytes)", sz_val, units[ui], (unsigned long long)p.size);
    draw_info_row("Size", sz);

    if (!p.location.empty()) draw_info_row("Location", p.location);

    draw_section("Ownership");
    draw_info_row("Owner", p.owner_name);
    draw_info_row("Group", p.group_name);
  } else {
    draw_info_row("Name", p.name);
    if (!p.mime_type.empty()) draw_info_row("Type", p.mime_type);
    if (p.is_dir) {
      uint64_t item_count = 0;
      std::error_code ec;
      for ([[maybe_unused]] auto& de : fs::directory_iterator(p.path, ec))
        if (!ec) ++item_count;
      draw_info_row("Contents", std::to_string(item_count) + " items");
    } else {
      char sz[64];
      double sz_val = static_cast<double>(p.size);
      const char* units[] = {"B", "KB", "MB", "GB", "TB"};
      int ui = 0;
      while (sz_val >= 1024.0 && ui < 4) { sz_val /= 1024.0; ++ui; }
      if (ui == 0)
        snprintf(sz, sizeof(sz), "%llu B", (unsigned long long)p.size);
      else
        snprintf(sz, sizeof(sz), "%.1f %s (%llu bytes)", sz_val, units[ui], (unsigned long long)p.size);
      draw_info_row("Size", sz);
    }
    // Always show Size (for directories too)
    if (p.is_dir) {
      char sz[64];
      double sz_val = static_cast<double>(p.size);
      const char* units[] = {"B", "KB", "MB", "GB", "TB"};
      int ui = 0;
      while (sz_val >= 1024.0 && ui < 4) { sz_val /= 1024.0; ++ui; }
      if (ui == 0)
        snprintf(sz, sizeof(sz), "%llu B", (unsigned long long)p.size);
      else
        snprintf(sz, sizeof(sz), "%.1f %s (%llu bytes)", sz_val, units[ui], (unsigned long long)p.size);
      draw_info_row("Size", sz);
    }
    char timebuf[64];
    if (p.modified_sec != 0) {
      struct tm* tm_local = localtime(&p.modified_sec);
      strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", tm_local);
      draw_info_row("Modified", timebuf);
    }
    if (p.accessed_sec != 0) {
      struct tm* tm_local = localtime(&p.accessed_sec);
      strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", tm_local);
      draw_info_row("Accessed", timebuf);
    }
    if (p.created_sec != 0) {
      struct tm* tm_local = localtime(&p.created_sec);
      strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", tm_local);
      draw_info_row("Created", timebuf);
    }
    if (!p.location.empty()) draw_info_row("Location", p.location);

    draw_section("Ownership");
    draw_info_row("Owner", p.owner_name);
    draw_info_row("Group", p.group_name);
  }

    if (p.can_be_executable) {
      ly += 4;
      draw_separator(cr, col1_x, ly, row_w);
      ly += 14;
      // Pill background for exec toggle row
      double exec_pill_r = (app.surface_r + app.bg_r) * 0.5 + 0.12;
      double exec_pill_g = (app.surface_g + app.bg_g) * 0.5 + 0.12;
      double exec_pill_b = (app.surface_b + app.bg_b) * 0.5 + 0.12;
      cairo_set_source_rgba(cr, exec_pill_r, exec_pill_g, exec_pill_b, 0.75);
      draw_rounded_rect(cr, col1_x - 14, ly + 2, row_w + 28, 28, 14);
      cairo_fill(cr);

      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12);
      cairo_move_to(cr, col1_x + 2, ly + 16);
      cairo_show_text(cr, "Allow executing file as program");
      int toggle_w = 38, toggle_h = 20;
      int toggle_x = col2_x - toggle_w;
      int toggle_y = ly + 4;
      p.hit_exec_toggle[0] = toggle_x; p.hit_exec_toggle[1] = toggle_y;
      p.hit_exec_toggle[2] = toggle_w; p.hit_exec_toggle[3] = toggle_h;
      cairo_set_source_rgba(cr, p.executable ? app.accent_r : app.outline_r,
                            p.executable ? app.accent_g : app.outline_g,
                            p.executable ? app.accent_b : app.outline_b, 0.55);
      draw_rounded_rect(cr, toggle_x, toggle_y, toggle_w, toggle_h, toggle_h / 2);
      cairo_fill(cr);
      double knob_x = p.executable ? toggle_x + toggle_w - toggle_h : toggle_x;
      cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
      cairo_arc(cr, knob_x + toggle_h / 2.0, toggle_y + toggle_h / 2.0, toggle_h / 2.0 - 2, 0, 2 * M_PI);
      cairo_fill(cr);
    }

  // ── Permissions tab ──
  } else if (content_tab == 1) {
    draw_section("Access");

    const char* perm_names[] = {"Owner", "Group", "Others"};
    int combo_vals[3] = {p.perm_owner, p.perm_group, p.perm_other};
    const char* combo_items[] = {"None", "Read-only", "Read & Write", "Read, Write & Exec"};
    int combo_h = 30;
    int combo_w = 160;

    for (int pi = 0; pi < 3; ++pi) {
      // Pill background for permission row
      double perm_pill_r = (app.surface_r + app.bg_r) * 0.5 + 0.12;
      double perm_pill_g = (app.surface_g + app.bg_g) * 0.5 + 0.12;
      double perm_pill_b = (app.surface_b + app.bg_b) * 0.5 + 0.12;
      cairo_set_source_rgba(cr, perm_pill_r, perm_pill_g, perm_pill_b, 0.75);
      draw_rounded_rect(cr, col1_x - 14, ly + 2, row_w + 28, 28, 14);
      cairo_fill(cr);

      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12);
      cairo_move_to(cr, col1_x + 2, ly + 18);
      cairo_show_text(cr, perm_names[pi]);

      int combo_x = col2_x - combo_w;
      int combo_y = ly;
      p.hit_combo[pi][0] = combo_x; p.hit_combo[pi][1] = combo_y;
      p.hit_combo[pi][2] = combo_w; p.hit_combo[pi][3] = combo_h;

      bool combo_hov = (app.pointerX >= combo_x && app.pointerX < combo_x + combo_w &&
                        app.pointerY >= combo_y && app.pointerY < combo_y + combo_h);
      bool combo_sel = (pi == p.combo_open);

      cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, combo_hov || combo_sel ? 0.1 : 0.04);
      draw_rounded_rect(cr, combo_x, combo_y, combo_w, combo_h, 6);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b,
                            combo_hov || combo_sel ? 0.45 : 0.25);
      cairo_set_line_width(cr, 1);
      draw_rounded_rect(cr, combo_x, combo_y, combo_w, combo_h, 6);
      cairo_stroke(cr);

      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12);
      cairo_move_to(cr, combo_x + 10, combo_y + 19);
      cairo_show_text(cr, combo_items[combo_vals[pi]]);

      // Arrow
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_set_line_width(cr, 1.5);
      int ax = combo_x + combo_w - 16;
      int ay = combo_y + combo_h / 2;
      cairo_move_to(cr, ax - 3, ay - 3);
      cairo_line_to(cr, ax, ay + 1);
      cairo_line_to(cr, ax + 3, ay - 3);
      cairo_stroke(cr);

      if (combo_sel) {
        int dd_item_h = 26;
        int dd_y = combo_y + combo_h + 3;
        int dd_h = 4 * dd_item_h;
        cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.97);
        draw_rounded_rect(cr, combo_x, dd_y, combo_w, dd_h, 6);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
        cairo_set_line_width(cr, 1);
        draw_rounded_rect(cr, combo_x, dd_y, combo_w, dd_h, 6);
        cairo_stroke(cr);
        for (int ci = 0; ci < 4; ++ci) {
          int item_y = dd_y + ci * dd_item_h;
          bool item_hov = (app.pointerX >= combo_x && app.pointerX < combo_x + combo_w &&
                           app.pointerY >= item_y && app.pointerY < item_y + dd_item_h);
          bool item_sel = (ci == combo_vals[pi]);
          p.hit_combo_items[pi][ci][0] = combo_x;
          p.hit_combo_items[pi][ci][1] = item_y;
          p.hit_combo_items[pi][ci][2] = combo_w;
          p.hit_combo_items[pi][ci][3] = dd_item_h;
          if (item_hov || item_sel) {
            cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, item_hov ? 0.18 : 0.08);
            draw_rounded_rect(cr, combo_x + 2, item_y + 1, combo_w - 4, dd_item_h - 2, 4);
            cairo_fill(cr);
          }
          cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
          cairo_set_font_size(cr, 12);
          cairo_move_to(cr, combo_x + 10, item_y + 17);
          cairo_show_text(cr, combo_items[ci]);
        }
      }
      ly += combo_h + 8;
    }

    if (!p.is_dir) {
      draw_section("Execution");
      // Pill background for exec toggle row in Permissions tab
      double exec_pill_r2 = (app.surface_r + app.bg_r) * 0.5 + 0.12;
      double exec_pill_g2 = (app.surface_g + app.bg_g) * 0.5 + 0.12;
      double exec_pill_b2 = (app.surface_b + app.bg_b) * 0.5 + 0.12;
      cairo_set_source_rgba(cr, exec_pill_r2, exec_pill_g2, exec_pill_b2, 0.75);
      draw_rounded_rect(cr, col1_x - 14, ly + 2, row_w + 28, 28, 14);
      cairo_fill(cr);

      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12);
      cairo_move_to(cr, col1_x + 2, ly + 16);
      cairo_show_text(cr, "Allow executing file as program");
      int toggle_w = 38, toggle_h = 20;
      int toggle_x = col2_x - toggle_w;
      int toggle_y = ly + 4;
      p.hit_exec_toggle[0] = toggle_x; p.hit_exec_toggle[1] = toggle_y;
      p.hit_exec_toggle[2] = toggle_w; p.hit_exec_toggle[3] = toggle_h;
      cairo_set_source_rgba(cr, p.executable ? app.accent_r : app.outline_r,
                            p.executable ? app.accent_g : app.outline_g,
                            p.executable ? app.accent_b : app.outline_b, 0.55);
      draw_rounded_rect(cr, toggle_x, toggle_y, toggle_w, toggle_h, toggle_h / 2);
      cairo_fill(cr);
      double knob_x = p.executable ? toggle_x + toggle_w - toggle_h : toggle_x;
      cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
      cairo_arc(cr, knob_x + toggle_h / 2.0, toggle_y + toggle_h / 2.0, toggle_h / 2.0 - 2, 0, 2 * M_PI);
      cairo_fill(cr);
    }

  // ── Image tab ──
  } else if (content_tab == 2) {
    char dim[48];
    snprintf(dim, sizeof(dim), "%d \u00d7 %d px", p.image_w, p.image_h);
    draw_info_row("Dimensions", dim);
    char area[48];
    snprintf(area, sizeof(area), "%d MP", (int)((p.image_w / 1000000.0) * (p.image_h / 1000000.0) * 100) / 100);
    draw_info_row("Megapixels", area);
    if (!p.mime_type.empty()) draw_info_row("Type", p.mime_type);
    if (!p.image_colorspace.empty()) draw_info_row("Color Space", p.image_colorspace);
    if (!p.image_bit_depth.empty()) draw_info_row("Bit Depth", p.image_bit_depth + " bit");
    if (p.image_has_alpha) draw_info_row("Alpha", "Yes");
    if (!p.image_compression.empty() && p.image_compression != "Undef" && p.image_compression != "Undefined")
      draw_info_row("Compression", p.image_compression);
    if (!p.image_resolution.empty()) draw_info_row("Resolution", p.image_resolution + " " + p.image_res_unit);

  // ── Media tab ──
  } else if (content_tab == 3) {
    if (p.media_duration > 0) {
      int total_sec = static_cast<int>(p.media_duration);
      int hrs = total_sec / 3600;
      int mins = (total_sec % 3600) / 60;
      int secs = total_sec % 60;
      char dur[32];
      if (hrs > 0) snprintf(dur, sizeof(dur), "%d:%02d:%02d", hrs, mins, secs);
      else snprintf(dur, sizeof(dur), "%d:%02d", mins, secs);
      draw_info_row("Duration", dur);
    }
    if (!p.container.empty()) draw_info_row("Container", p.container);
    if (p.has_video) {
      if (!p.video_codec.empty()) {
        std::string vc = p.video_codec;
        if (!vc.empty()) vc[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(vc[0])));
        draw_info_row("Video Codec", vc);
      }
      if (p.video_w > 0 && p.video_h > 0) {
        char vdim[48];
        snprintf(vdim, sizeof(vdim), "%d \u00d7 %d px", p.video_w, p.video_h);
        draw_info_row("Dimensions", vdim);
      }
      if (!p.video_framerate.empty()) draw_info_row("Frame Rate", p.video_framerate + " fps");
      if (p.video_bitrate > 0) {
        char vbr[32];
        if (p.video_bitrate >= 1000000) snprintf(vbr, sizeof(vbr), "%.0f Mbps", p.video_bitrate / 1000000.0);
        else snprintf(vbr, sizeof(vbr), "%d kbps", p.video_bitrate / 1000);
        draw_info_row("Video Bitrate", vbr);
      }
    }
    if (p.has_audio) {
      if (!p.audio_codec.empty()) {
        std::string ac = p.audio_codec;
        if (!ac.empty()) ac[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(ac[0])));
        draw_info_row("Audio Codec", ac);
      }
      if (p.audio_sample_rate > 0) {
        char sr[32];
        snprintf(sr, sizeof(sr), "%d Hz", p.audio_sample_rate);
        draw_info_row("Sample Rate", sr);
      }
      if (p.audio_channels > 0) {
        static const char* ch_names[] = {"Mono", "Stereo", "3.0", "4.0", "5.0", "5.1", "6.1", "7.1"};
        std::string ch_str = (p.audio_channels >= 1 && p.audio_channels <= 8)
          ? ch_names[p.audio_channels - 1]
          : std::to_string(p.audio_channels) + " channels";
        draw_info_row("Channels", ch_str);
      }
      if (p.audio_bitrate > 0) {
        char abr[32];
        if (p.audio_bitrate >= 1000000) snprintf(abr, sizeof(abr), "%.0f Mbps", p.audio_bitrate / 1000000.0);
        else snprintf(abr, sizeof(abr), "%d kbps", p.audio_bitrate / 1000);
        draw_info_row("Audio Bitrate", abr);
      }
    }
  }

  p.content_h = ly - (content_y0 - p.scroll_px);
  cairo_restore(cr);

  // ── Bottom close button (right-aligned, clean style) ──
  int btn_w = 90;
  int btn_h = 32;
  int btn_x = cx + card_w - pad - btn_w;
  int btn_y = cy + card_h - 48;
  bool btn_hov = (app.pointerX >= btn_x && app.pointerX < btn_x + btn_w &&
                  app.pointerY >= btn_y && app.pointerY < btn_y + btn_h);
  p.hit_close_btn[0] = btn_x; p.hit_close_btn[1] = btn_y;
  p.hit_close_btn[2] = btn_w; p.hit_close_btn[3] = btn_h;

  cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, btn_hov ? 0.1 : 0.04);
  draw_rounded_rect(cr, btn_x, btn_y, btn_w, btn_h, 8);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, btn_x, btn_y, btn_w, btn_h, 8);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_text_extents(cr, "Close", &te);
  cairo_move_to(cr, btn_x + (btn_w - te.x_advance) / 2, btn_y + btn_h / 2 + te.height * 0.35);
  cairo_show_text(cr, "Close");
}

// ── Info panel (F11) ────────────────────────────────────────────

void draw_info_panel(AppState& app, cairo_t* cr) {
  if (!app.info_panel_open) return;

  double zf = app.zoom_pct / 100.0;
  int pw = app.info_panel_width;
  int px = app.width - pw;
  int top_h = app.top_bar_height + app.tab_bar_height;
  int ph = app.height - top_h - app.status_bar_height;
  int py = top_h;

  // Background (same tinted surface as sidebar)
  cairo_set_source_rgba(cr, app.surface_r * 2, app.surface_g * 2, app.surface_b * 2, 0.95);
  cairo_rectangle(cr, px, py, pw, ph);
  cairo_fill(cr);

  // Left separator
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_rectangle(cr, px, py, 1, ph);
  cairo_fill(cr);

  // ── Tab bar ──
  static const char* kTabNames[] = {"Preview", "Properties", "Terminal"};
  int tab_h = static_cast<int>(38 * zf);
  int tab_w = pw / 3;

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12 * zf);

  for (int i = 0; i < 3; ++i) {
    int tx = px + i * tab_w;
    app.info_panel_hit_tabs[i][0] = static_cast<double>(tx);
    app.info_panel_hit_tabs[i][1] = static_cast<double>(py);
    app.info_panel_hit_tabs[i][2] = static_cast<double>(tab_w);
    app.info_panel_hit_tabs[i][3] = static_cast<double>(tab_h);

    if (i == app.info_panel_tab) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
      cairo_rectangle(cr, static_cast<double>(tx), static_cast<double>(py),
                      static_cast<double>(tab_w), static_cast<double>(tab_h));
      cairo_fill(cr);
    }

    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                          i == app.info_panel_tab ? 0.95 : 0.55);
    cairo_text_extents_t te;
    cairo_text_extents(cr, kTabNames[i], &te);
    cairo_move_to(cr, tx + (tab_w - te.x_advance) / 2.0,
                  py + tab_h / 2.0 + te.height * 0.35);
    cairo_show_text(cr, kTabNames[i]);

    if (i == app.info_panel_tab) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
      cairo_rectangle(cr, tx + 6.0, py + tab_h - 2.5, tab_w - 12.0, 2.5);
      cairo_fill(cr);
    }
  }

  // Tab underline
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.15);
  cairo_rectangle(cr, static_cast<double>(px), static_cast<double>(py + tab_h),
                  static_cast<double>(pw), 1);
  cairo_fill(cr);

  int content_y = py + tab_h + 1;
  int content_h = ph - tab_h - 1;

  // ── Preview tab ──
  if (app.info_panel_tab == 0) {
    if (app.info_panel_path.empty() || app.info_panel_is_dir) {
      cairo_set_font_size(cr, 12 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 0.55);
      const char* msg = app.info_panel_path.empty() ? "No file selected"
                       : app.info_panel_is_dir ? "(folder)"
                       : "";
      if (*msg) {
        cairo_text_extents_t te;
        cairo_text_extents(cr, msg, &te);
        cairo_move_to(cr, px + (pw - te.x_advance) / 2.0,
                      content_y + content_h / 2.0);
        cairo_show_text(cr, msg);
      }
    } else {
      int thumb_px = pw - 24;
      cairo_surface_t* thumb = get_thumbnail(app, app.info_panel_path, thumb_px);
      if (thumb) {
        int tw = cairo_image_surface_get_width(thumb);
        int th = cairo_image_surface_get_height(thumb);
        if (tw > 0 && th > 0) {
          int avail_h = content_h - 50;
          double s = std::min(1.0, std::min(static_cast<double>(thumb_px) / tw,
                                            static_cast<double>(avail_h) / th));
          int dw = static_cast<int>(tw * s);
          int dh = static_cast<int>(th * s);
          int dx = px + (pw - dw) / 2;
          int dy = content_y + (avail_h - dh) / 2;
          cairo_save(cr);
          cairo_rectangle(cr, static_cast<double>(dx), static_cast<double>(dy),
                          static_cast<double>(dw), static_cast<double>(dh));
          cairo_clip(cr);
          cairo_set_source_surface(cr, thumb, static_cast<double>(dx),
                                   static_cast<double>(dy));
          cairo_paint(cr);
          cairo_restore(cr);
        }
      }
      // File name below preview
      std::string name = app.info_panel_name;
      if (name.size() > 24) {
        auto dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0) {
          std::string ext = name.substr(dot);
          name = name.substr(0, 21 - ext.size()) + "..." + ext;
        } else {
          name = name.substr(0, 21) + "...";
        }
      }
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.85);
      cairo_set_font_size(cr, 11 * zf);
      cairo_text_extents_t te;
      cairo_text_extents(cr, name.c_str(), &te);
      cairo_move_to(cr, px + (pw - te.x_advance) / 2.0,
                    py + ph - 14);
      cairo_show_text(cr, name.c_str());
    }
  }

  // ── Properties tab ──
  else if (app.info_panel_tab == 1) {
    auto fmt_size = [](uint64_t bytes) -> std::string {
      if (bytes < 1024ULL) return std::to_string(bytes) + " B";
      if (bytes < 1024ULL * 1024) return std::to_string(bytes / 1024) + " KB";
      if (bytes < 1024ULL * 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
      return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
    };
    auto fmt_date = [](int64_t sec) -> std::string {
      char buf[32];
      struct tm tm;
      localtime_r(&sec, &tm);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
      return buf;
    };

    int ly = content_y + 16;
    int margin = 10;
    int col1_x = px + margin;
    int col2_x = px + pw / 2 + 4;

    auto draw_row = [&](const char* label, const std::string& value) {
      cairo_set_font_size(cr, 11 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 0.7);
      cairo_move_to(cr, static_cast<double>(col1_x), static_cast<double>(ly));
      cairo_show_text(cr, label);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
      cairo_move_to(cr, static_cast<double>(col2_x), static_cast<double>(ly));
      cairo_show_text(cr, value.c_str());
      ly += 22;
    };

    draw_row("Name", app.info_panel_name);
    draw_row("Size", fmt_size(app.info_panel_size));

    // File type
    if (app.cur_tab().selected_idx >= 0) {
      int si = app.cur_tab().selected_idx;
      int ri = app.cur_tab().visible_entries[si];
      if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size())) {
        static const char* kTypeNames[] = {"Folder", "Image", "Audio", "Video", "Text",
                                           "Markdown", "Code", "Document", "Font",
                                           "Archive", "Executable", "Web", "File"};
        int ti = static_cast<int>(app.cur_tab().entries[ri].type);
        if (ti >= 0 && ti < 13)
          draw_row("Type", kTypeNames[ti]);
      }
    }

    draw_row("Modified", fmt_date(app.info_panel_modified_sec));
    draw_row("Owner", app.info_panel_owner);
    draw_row("Group", app.info_panel_group);
    if (!app.info_panel_mime_type.empty())
      draw_row("MIME", app.info_panel_mime_type);

    // Permissions string
    {
      mode_t m = app.info_panel_mode;
      char perm[11] = {};
      perm[0] = S_ISDIR(m) ? 'd' : '-';
      perm[1] = (m & S_IRUSR) ? 'r' : '-';
      perm[2] = (m & S_IWUSR) ? 'w' : '-';
      perm[3] = (m & S_IXUSR) ? 'x' : '-';
      perm[4] = (m & S_IRGRP) ? 'r' : '-';
      perm[5] = (m & S_IWGRP) ? 'w' : '-';
      perm[6] = (m & S_IXGRP) ? 'x' : '-';
      perm[7] = (m & S_IROTH) ? 'r' : '-';
      perm[8] = (m & S_IWOTH) ? 'w' : '-';
      perm[9] = (m & S_IXOTH) ? 'x' : '-';
      draw_row("Permissions", perm);
    }
  }

  // ── Terminal tab (stub) ──
  else if (app.info_panel_tab == 2) {
    cairo_set_font_size(cr, 12 * zf);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 0.55);
    const char* msg = "Terminal (not implemented)";
    cairo_text_extents_t te;
    cairo_text_extents(cr, msg, &te);
    cairo_move_to(cr, px + (pw - te.x_advance) / 2.0,
                  content_y + content_h / 2.0);
    cairo_show_text(cr, msg);
  }
}

// ── Operations panel (right sidebar) ────────────────────────────

void draw_operations_panel(AppState& app, cairo_t* cr) {
  if (app.ops_panel_slide < 0.01) return;
  if (!app.op_progress) return;

  double zf = app.zoom_pct / 100.0;
  int pw = app.ops_panel_width;
  int slide_w = static_cast<int>(pw * app.ops_panel_slide);
  int px = app.width - slide_w;
  int top_h = app.top_bar_height + app.tab_bar_height;
  int ph = app.height - top_h - app.status_bar_height;
  int py = top_h;

  // Background
  cairo_set_source_rgba(cr, app.surface_r * 2, app.surface_g * 2, app.surface_b * 2, 0.95);
  cairo_rectangle(cr, px, py, slide_w, ph);
  cairo_fill(cr);

  // Left separator
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_rectangle(cr, px, py, 1, ph);
  cairo_fill(cr);

  if (app.ops_panel_slide < 0.5) return;

  auto& p = *app.op_progress;
  int total = p.total_files;
  int done = p.copied_files;
  bool counting = total == 0 && p.active;

  int content_x = px + 16;
  int content_w = slide_w - 32;
  int y = py + 24;

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

  // ── Header ──
  {
    const char* hdr = "FILE OPERATIONS";
    if (p.type == OperationType::Extract) hdr = "EXTRACTION";
    else if (p.type == OperationType::Move) hdr = "MOVE";
    else hdr = "COPY";
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.6);
    cairo_set_font_size(cr, 11.0 * zf);
    cairo_move_to(cr, content_x, y + 12 * zf);
    cairo_show_text(cr, hdr);
    y += 28 * static_cast<int>(zf);
  }

  // ── Current file ──
  {
    std::string label;
    if (p.type == OperationType::Extract)
      label = "Extracting";
    else if (p.type == OperationType::Move)
      label = "Moving";
    else
      label = "Copying";
    if (!p.current_file.empty()) {
      label += " ";
      std::string fname = p.current_file;
      int max_chars = static_cast<int>(content_w / (7.0 * zf));
      if (max_chars < 10) max_chars = 10;
      if (static_cast<int>(fname.size()) > max_chars) {
        auto dot = fname.rfind('.');
        if (dot != std::string::npos && dot > 0) {
          std::string ext = fname.substr(dot);
          int keep = max_chars - 3 - static_cast<int>(ext.size());
          if (keep > 0)
            fname = fname.substr(0, static_cast<size_t>(keep)) + "..." + ext;
          else
            fname = fname.substr(0, static_cast<size_t>(max_chars - 3)) + "...";
        } else {
          fname = fname.substr(0, static_cast<size_t>(max_chars - 3)) + "...";
        }
      }
      label += fname;
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13.0 * zf);
    cairo_move_to(cr, content_x, y + 14 * zf);
    cairo_show_text(cr, label.c_str());
    y += 30 * static_cast<int>(zf);
  }

  // ── Progress bar ──
  {
    int bar_h = static_cast<int>(10 * zf);
    double bar_pct = counting ? 0.0 : (total > 0 ? static_cast<double>(done) / total : 0.0);
    int fill_w = static_cast<int>(content_w * bar_pct);

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
    draw_rounded_rect(cr, content_x, y, content_w, bar_h, static_cast<int>(4 * zf));
    cairo_fill(cr);

    if (fill_w > 0) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.8);
      draw_rounded_rect(cr, content_x, y, fill_w, bar_h, static_cast<int>(4 * zf));
      cairo_fill(cr);
    }
    y += bar_h + 12 * static_cast<int>(zf);
  }

  // ── File count ──
  {
    char buf[64];
    if (counting) {
      std::snprintf(buf, sizeof(buf), "Counting files\u2026");
    } else {
      int pct = total > 0 ? static_cast<int>(100.0 * done / total) : 0;
      std::snprintf(buf, sizeof(buf), "%d%%  (%d / %d files)", pct, done, total);
    }
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
    cairo_set_font_size(cr, 11.0 * zf);
    cairo_move_to(cr, content_x, y + 12 * zf);
    cairo_show_text(cr, buf);
    y += 22 * static_cast<int>(zf);
  }

  // ── Speed ──
  if (p.total_bytes.load() > 0) {
    double speed = p.speed_mbps();
    char buf[64];
    if (speed < 0.01)
      std::snprintf(buf, sizeof(buf), "Calculating speed\u2026");
    else
      std::snprintf(buf, sizeof(buf), "%.1f MB/s", speed);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
    cairo_set_font_size(cr, 11.0 * zf);
    cairo_move_to(cr, content_x, y + 12 * zf);
    cairo_show_text(cr, buf);
    y += 22 * static_cast<int>(zf);

    // ── Time remaining ──
    if (p.active) {
      double eta = p.eta_seconds();
      if (eta >= 0) {
        int mins = static_cast<int>(eta) / 60;
        int secs = static_cast<int>(eta) % 60;
        char eta_buf[64];
        if (mins > 0)
          std::snprintf(eta_buf, sizeof(eta_buf), "~%dm %ds remaining", mins, secs);
        else
          std::snprintf(eta_buf, sizeof(eta_buf), "~%ds remaining", secs);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, 0.7);
        cairo_set_font_size(cr, 10.0 * zf);
        cairo_move_to(cr, content_x, y + 12 * zf);
        cairo_show_text(cr, eta_buf);
        y += 20 * static_cast<int>(zf);
      }
    }
  }

  // ── Cancel button ──
  {
    int btn_size = static_cast<int>(18 * zf);
    int btn_x = px + slide_w - btn_size - 12;
    int btn_y = py + 10;
    app.ops_cancel_x = btn_x;
    app.ops_cancel_y = btn_y;
    app.ops_cancel_w = btn_size;
    app.ops_cancel_h = btn_size;

    int pad = static_cast<int>(4 * zf);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.5);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, btn_x + pad, btn_y + pad);
    cairo_line_to(cr, btn_x + btn_size - pad, btn_y + btn_size - pad);
    cairo_move_to(cr, btn_x + btn_size - pad, btn_y + pad);
    cairo_line_to(cr, btn_x + pad, btn_y + btn_size - pad);
    cairo_stroke(cr);
  }
}

// ── Build tree view entries ──────────────────────────────────────
void build_tree_entries(AppState& app) {
  auto& tab = app.cur_tab();
  tab.tree_entries.clear();
  tab.tree_entries.reserve(tab.visible_entries.size());
  for (int vi : tab.visible_entries) {
    auto& entry = tab.entries[vi];
    bool is_expanded = tab.tree_expanded.count(entry.path) > 0;
    bool has_children = entry.is_dir;
    TreeEntry te{entry.name, entry.path, entry.is_dir, 0, has_children, is_expanded};
    tab.tree_entries.push_back(std::move(te));
    if (entry.is_dir && is_expanded) {
      // Read children recursively
      std::vector<TreeEntry> children;
      std::error_code ec;
      const auto hidden_names = read_hidden_file(entry.path);
      for (auto& de : fs::directory_iterator(entry.path, ec)) {
        auto path = de.path();
        auto name = path.filename().string();
        if (name.empty()) continue;
        if (!app.show_hidden &&
            (name[0] == '.' || hidden_names.count(name) > 0)) continue;
        bool is_dir = de.is_directory(ec);
        bool child_expanded = tab.tree_expanded.count(path.string()) > 0;
        children.push_back({name, path.string(), is_dir, 1, is_dir, child_expanded});
      }
      std::sort(children.begin(), children.end(), [](auto& a, auto& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return strverscmp(a.name.c_str(), b.name.c_str()) < 0;
      });
      for (auto& child : children) {
        if (child.is_dir && child.is_expanded) {
          std::vector<TreeEntry> grandchildren;
          const auto ghidden = read_hidden_file(child.path);
          for (auto& gde : fs::directory_iterator(child.path, ec)) {
            auto gp = gde.path();
            auto gn = gp.filename().string();
            if (gn.empty()) continue;
            if (!app.show_hidden &&
                (gn[0] == '.' || ghidden.count(gn) > 0)) continue;
            bool gd = gde.is_directory(ec);
            grandchildren.push_back({gn, gp.string(), gd, 2, gd, false});
          }
          std::sort(grandchildren.begin(), grandchildren.end(), [](auto& a, auto& b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return strverscmp(a.name.c_str(), b.name.c_str()) < 0;
          });
          for (auto& gc : grandchildren)
            tab.tree_entries.push_back(std::move(gc));
        }
        tab.tree_entries.push_back(std::move(child));
      }
    }
  }
}

// ── Tree view ────────────────────────────────────────────────────
void draw_tree_view(AppState& app, cairo_t* cr, int content_x,
                    int content_y, int content_w, int view_h) {
  double zf = app.zoom_pct / 100.0;
  int entry_h = static_cast<int>(28.0 * zf);
  int icon_size = static_cast<int>(20.0 * zf);
  int indent_step = static_cast<int>(24.0 * zf);
  int arrow_w = static_cast<int>(16.0 * zf);

  build_tree_entries(app);

  int y = content_y - app.cur_tab().scroll_px;

  for (int vi = 0; vi < static_cast<int>(app.cur_tab().tree_entries.size()); ++vi) {
    auto& te = app.cur_tab().tree_entries[vi];
    int indent = te.depth * indent_step;

    if (y + entry_h < content_y) { y += entry_h; continue; }
    if (y > content_y + view_h) break;

    // Map vi to visible_entries index for selection
    int visible_idx = -1;
    if (te.depth == 0) {
      if (vi < static_cast<int>(app.cur_tab().visible_entries.size()))
        visible_idx = vi;
    }

    bool selected = false;
    if (visible_idx >= 0) {
      selected = visible_idx == app.cur_tab().selected_idx ||
                 std::find(app.cur_tab().multi_selected.begin(),
                           app.cur_tab().multi_selected.end(), visible_idx)
                     != app.cur_tab().multi_selected.end();
    }
    bool hovered = vi == app.cur_tab().hover_idx;

    if (selected) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.25);
      cairo_rectangle(cr, content_x, y, content_w, entry_h);
      cairo_fill(cr);
    } else if (hovered) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.06);
      cairo_rectangle(cr, content_x, y, content_w, entry_h);
      cairo_fill(cr);
    }

    // Expand/collapse arrow for directories
    int arrow_x = content_x + indent + 4;
    int arrow_y = y + (entry_h - arrow_w) / 2;
    if (te.is_dir) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.6);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, arrow_w * 0.8);
      cairo_move_to(cr, arrow_x, arrow_y + arrow_w * 0.75);
      cairo_show_text(cr, te.is_expanded ? "\u25BC" : "\u25B6");
    }

    // File icon
    int icon_x = content_x + indent + arrow_w + 4;
    int icon_y = y + (entry_h - icon_size) / 2;
    FileType ftype = te.is_dir ? FileType::Folder : FileType::File;
    draw_file_icon_cairo(app, cr, icon_x, icon_y, icon_size, ftype, selected, "", nullptr, &te.path);

    // Name
    int text_x = icon_x + icon_size + 6;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0 * zf);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, text_x, y + entry_h / 2 + 4);
    std::string display = te.name;
    cairo_text_extents_t te2;
    cairo_text_extents(cr, display.c_str(), &te2);
    if (te2.width > content_w - (text_x - content_x) - 10) {
      while (!display.empty()) {
        cairo_text_extents(cr, (display + "...").c_str(), &te2);
        if (te2.width <= content_w - (text_x - content_x) - 10) break;
        display.pop_back();
      }
      display += "...";
    }
    cairo_show_text(cr, display.c_str());

    y += entry_h;
  }

  app.cur_tab().content_h = y - content_y + app.cur_tab().scroll_px;
}

// ── Compact view ─────────────────────────────────────────────────
void draw_compact_view(AppState& app, cairo_t* cr, int content_x,
                       int content_y, int content_w, int view_h) {
  double zf = app.zoom_pct / 100.0;
  int entry_h = static_cast<int>(24.0 * zf);
  int icon_size = static_cast<int>(16.0 * zf);
  int text_x = content_x + static_cast<int>(28.0 * zf);

  int y = content_y - app.cur_tab().scroll_px;

  std::string prev_group;
  int cheader_h = static_cast<int>(20.0 * zf);
  std::unordered_map<std::string, int> cheader_ys;
  bool sticky_recorded = false;
  std::string first_vis_label;
  int first_vis_header_y = INT_MIN;

  for (int vi = 0; vi < static_cast<int>(app.cur_tab().visible_entries.size()); ++vi) {
    int real_idx = app.cur_tab().visible_entries[vi];
    if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
      continue;
    auto& entry = app.cur_tab().entries[real_idx];

    std::string row_label;
    if (app.cur_tab().group_field > 0) {
      row_label = group_label_for(app, entry);
      if (row_label != prev_group) {
        prev_group = row_label;
        cheader_ys[row_label] = y;
        if (y + cheader_h >= content_y)
          draw_group_header_band(app, cr, content_x, y, content_w, cheader_h,
                                  row_label);
        y += cheader_h;
      }
      if (!sticky_recorded && y + entry_h >= content_y) {
        sticky_recorded = true;
        first_vis_label = row_label;
        auto it = cheader_ys.find(row_label);
        first_vis_header_y = (it != cheader_ys.end()) ? it->second : INT_MIN;
      }
    }

    if (y + entry_h < content_y) { y += entry_h; continue; }
    if (y > content_y + view_h) break;

    bool selected = vi == app.cur_tab().selected_idx ||
                    std::find(app.cur_tab().multi_selected.begin(),
                              app.cur_tab().multi_selected.end(), vi)
                        != app.cur_tab().multi_selected.end();
    bool hovered = vi == app.cur_tab().hover_idx;

    if (selected) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.25);
      cairo_rectangle(cr, content_x, y, content_w, entry_h);
      cairo_fill(cr);
    } else if (hovered) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.06);
      cairo_rectangle(cr, content_x, y, content_w, entry_h);
      cairo_fill(cr);
    }

    cairo_surface_t* thumb = nullptr;
    if (entry.type == FileType::Image || entry.type == FileType::Video) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    } else if (entry.type == FileType::Document && (is_pdf_extension(entry.path) || is_epub_extension(entry.path))) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    }
    draw_file_icon_cairo(app, cr, content_x + 6, y + (entry_h - icon_size) / 2,
                          icon_size, entry.type, selected, entry.icon_name, thumb, &entry.path, &entry);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.0 * zf);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, text_x, y + entry_h / 2 + 4);

    std::string display = entry.name;
    cairo_text_extents_t te;
    cairo_text_extents(cr, display.c_str(), &te);
    if (te.width > content_w - (text_x - content_x) - 6) {
      while (!display.empty()) {
        cairo_text_extents(cr, (display + "...").c_str(), &te);
        if (te.width <= content_w - (text_x - content_x) - 6) break;
        display.pop_back();
      }
      display += "...";
    }
    cairo_show_text(cr, display.c_str());

    y += entry_h;
  }

  // Sticky group header pinned to the top of the viewport
  if (app.cur_tab().group_field > 0 && sticky_recorded &&
      first_vis_header_y != INT_MIN && first_vis_header_y < content_y)
    draw_group_header_band(app, cr, content_x, content_y, content_w,
                            cheader_h, first_vis_label, true);

  app.cur_tab().content_h = y - content_y + app.cur_tab().scroll_px;
}

// ── Hit-test: tree view ──────────────────────────────────────────
int hit_test_tree(AppState& app, int x, int y, bool for_click) {
  auto& tab = app.cur_tab();
  if (tab.tree_entries.empty()) build_tree_entries(app);
  if (tab.tree_entries.empty()) return -1;

  double zf = app.zoom_pct / 100.0;
  int entry_h = static_cast<int>(28.0 * zf);
  int indent_step = static_cast<int>(24.0 * zf);
  int arrow_w = static_cast<int>(16.0 * zf);

  int content_x, content_y;
  {
    int sidebar_w = app.sidebar_expanded ? app.sidebar_width : 0;
    int info_panel_w = app.info_panel_open ? app.info_panel_width : 0;
    content_x = sidebar_w;
    (void)info_panel_w; (void)sidebar_w;

    if (app.split_view) {
      int s_w = app.sidebar_expanded ? app.sidebar_width : 0;
      int content_x_global = s_w;
      int content_w_global = app.width - s_w - (app.info_panel_open ? app.info_panel_width : 0);
      int split = app.split_divider_x;
      if (split <= 0) split = content_w_global / 2;
      int div_w = 4;
      int left_w = std::max(100, split - div_w / 2);
      int right_x = std::min(content_x_global + content_w_global - 100, content_x_global + split + div_w / 2);
      (void)left_w;
      if (app.active_pane == 0)
        content_x = content_x_global;
      else
        content_x = right_x;
    }
    content_y = app.top_bar_height + app.tab_bar_height;
    if (app.split_view) content_y += app.top_bar_height;
  }

  int rel_y = y - content_y + tab.scroll_px;
  int idx = rel_y / entry_h;
  if (idx < 0 || idx >= static_cast<int>(tab.tree_entries.size())) return -1;

  // Check if click is on expand/collapse arrow
  auto& te = tab.tree_entries[idx];
  int indent = te.depth * indent_step;
  int arrow_x_min = content_x + indent + 4;
  int arrow_x_max = arrow_x_min + arrow_w;
  int arrow_y = content_y + idx * entry_h - tab.scroll_px + (entry_h - arrow_w) / 2;
  if (te.is_dir && x >= arrow_x_min && x < arrow_x_max &&
      y >= arrow_y && y < arrow_y + arrow_w) {
    if (for_click) {
      if (tab.tree_expanded.count(te.path))
        tab.tree_expanded.erase(te.path);
      else
        tab.tree_expanded.insert(te.path);
      tab.hover_idx = idx;
    }
    return -2; // arrow hit
  }

  return idx;
}

// ── Hit-test: compact view ───────────────────────────────────────
int hit_test_compact(AppState& app, int x, int y) {
  auto& tab = app.cur_tab();
  if (tab.visible_entries.empty()) return -1;

  double zf = app.zoom_pct / 100.0;
  int entry_h = static_cast<int>(24.0 * zf);

  int content_y = app.top_bar_height + app.tab_bar_height;
  if (app.split_view) content_y += app.top_bar_height;

  int rel_y = y - content_y + tab.scroll_px;
  int idx = rel_y / entry_h;
  if (idx < 0 || idx >= static_cast<int>(tab.visible_entries.size())) return -1;
  return idx;
}

} // namespace eh::file_browser
