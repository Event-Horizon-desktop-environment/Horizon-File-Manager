// draw_file_icons.cpp — icon-name lookup, file-icon body and status-badge drawing.
// Moved wholesale from ui/draw.cpp (byte-identical bodies).

#include "../app.hpp"
#include "../features/sidebar.hpp"
#include "app/file_browser/features/thumb_pool.hpp"
#include "draw_file_icons.hpp"

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
#include "app/file_browser/ui/pixblit.hpp"
#include "app/file_browser/features/pdf_preview.hpp"
#include "app/file_browser/features/epub_preview.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {

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

static bool is_pdf_ext(const std::string& path) {
  if (path.size() < 5) return false;
  const char* s = path.c_str();
  size_t i = path.size() - 4;
  return (s[i] == '.' || s[i] == '.') &&
         (s[i+1] == 'p' || s[i+1] == 'P') &&
         (s[i+2] == 'd' || s[i+2] == 'D') &&
         (s[i+3] == 'f' || s[i+3] == 'F');
}

const char* icon_name_for_file_type(FileType ft, const std::string* file_path) {
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
                                  const std::string* file_path = nullptr,
                                  BatchedBlitter* bb = nullptr) {
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
        if (fabs(size - std::max(iw, ih)) < 0.5) {
          if (bb && bb->add(ic->surface, x + static_cast<int>((size - iw) / 2.0),
                            y + static_cast<int>((size - ih) / 2.0)))
            return;
          cairo_set_source_surface(cr, ic->surface, x + (size - iw) / 2.0,
                                   y + (size - ih) / 2.0);
          cairo_paint(cr);
          return;
        }
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
      // Exact-fit fast path: the icon cache now hands back an icon rasterized
      // at the precise draw size (buckets match), so scale == 1.0. Skip the
      // translate/scale transform and blit the surface straight: pixman's
      // non-transformed path is substantially cheaper per pixel, and at 4K
      // with hundreds of visible cells the per-blit cairo state setup is the
      // cost that matters most.
      if (fabs(size - std::max(iw, ih)) < 0.5) {
        if (bb && bb->add(ic->surface, x + static_cast<int>((size - iw) / 2.0),
                          y + static_cast<int>((size - ih) / 2.0)))
          return;
        cairo_set_source_surface(cr, ic->surface, x + (size - iw) / 2.0,
                                 y + (size - ih) / 2.0);
        cairo_paint(cr);
        return;
      }
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
void draw_file_icon_cairo(AppState& app, cairo_t* cr, int x, int y,
                          int size, FileType ft, bool selected,
                          const std::string& icon_name,
                          cairo_surface_t* thumb,
                          const std::string* file_path,
                          const FileEntry* entry,
                          BatchedBlitter* bb) {
  // Emblems must sit on top of the icon, but the BatchedBlitter composites
  // its whole queue in one LATER pass — it would paint the icon over an
  // emblem drawn now. Entries that carry an emblem therefore draw their icon
  // through plain cairo (immediately), so the emblem lands in front. The fast
  // batch path is preserved for the emblem-free majority.
  bool locked_by_root = entry && entry->owned_by_root && geteuid() != 0;
  bool has_emblem = entry && size >= 24 &&
                    (entry->is_symlink || locked_by_root ||
                     (entry->readable && !entry->writable) ||
                     (!entry->readable || (entry->is_dir && entry->mode &&
                                           !(entry->mode & S_IXUSR))));

  draw_file_icon_body(app, cr, x, y, size, ft, selected, icon_name,
                      thumb, file_path, has_emblem ? nullptr : bb);
  if (!entry) return;

  // Skip emblems on tiny icons
  if (size < 24) return;

  double r = std::max(5.0, size * 0.11);
  double pad = r * 0.4;
  double cy = y + size - r - pad;
  double cx = x + r + pad;
  int drawn = 0;

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

} // namespace eh::file_browser
