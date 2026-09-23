#include "../app.hpp"
#include "../trace.hpp"
#include "../features/sidebar/sidebar.hpp"
#include "../features/view_zoom/view_zoom.hpp"
#include "app/file_browser/features/thumbnails/thumb_pool.hpp"
#include "app/file_browser/features/dir_stats/dir_stats.hpp"

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

#include "draw_helpers.hpp"
#include "draw_file_icons.hpp"
#include "draw_thumbnails.hpp"
#include "layout.hpp"

#include "platform/common/icon_cache/icon_cache.hpp"
#include "app/file_browser/features/preview/svg_preview.hpp"
#include "app/file_browser/features/preview/video_preview.hpp"
#include "app/file_browser/features/preview/pdf_preview.hpp"
#include "app/file_browser/features/preview/epub_preview.hpp"
#include "app/file_browser/features/preview/image_preview.hpp"
#include "app/file_browser/features/thumbnails/thumbnail_cache.hpp"
#include "app/file_browser/ui/pixblit.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {

// ── Drop action chooser (Copy/Move prompt) ──────────────────────

namespace {

constexpr int kChooserW = 200;
constexpr int kChooserHeaderH = 28;
constexpr int kChooserSepH = 9;
// Row 0 = "Copy here", row 1 = "Move here".
constexpr int kChooserRowH = 34;
constexpr int kChooserH = kChooserHeaderH + kChooserSepH + 2 * kChooserRowH;

struct DropChooserGeometry {
  int x = 0, y = 0, w = kChooserW, h = kChooserH;
};

DropChooserGeometry drop_chooser_geometry(const AppState& app) {
  DropChooserGeometry g;
  g.x = app.drop_chooser_x;
  g.y = app.drop_chooser_y;
  clamp_popup_rect(app, g.x, g.y, g.w, g.h);
  return g;
}

} // namespace

void draw_drop_chooser(AppState& app, cairo_t* cr) {
  if (!app.drop_chooser_open) return;
  const DropChooserGeometry g = drop_chooser_geometry(app);
  int cx = g.x;
  int cy = g.y;
  int w = g.w;

  // Drop shadow (3 layers, lighter than the context menu)
  for (int s = 3; s >= 0; --s) {
    double a = 0.12 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, cx + s * 2.5, cy + s * 3, w, kChooserH, 10);
    cairo_fill(cr);
  }

  // Card background
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
  draw_rounded_rect(cr, cx, cy, w, kChooserH, 10);
  cairo_fill_preserve(cr);

  // Border
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.30);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  int ry = cy;

  // Header: item count
  int n = static_cast<int>(app.drop_chooser_srcs.size());
  std::string head = (n == 1) ? "1 item" : std::to_string(n) + " items";
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 0.85);
  cairo_text_extents_t te;
  cairo_text_extents(cr, head.c_str(), &te);
  double ty = ry + (kChooserHeaderH - te.height) / 2.0 - te.y_bearing;
  cairo_move_to(cr, cx + 14, ty);
  cairo_show_text(cr, head.c_str());
  ry += kChooserHeaderH;

  // Separator
  ry += 4;
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.18);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, cx + 14, ry + 0.5);
  cairo_line_to(cr, cx + w - 14, ry + 0.5);
  cairo_stroke(cr);
  ry += 5;

  // Action rows: 0 = copy, 1 = move
  static const char* kDropActions[2] = {"Copy here", "Move here"};
  for (int i = 0; i < 2; ++i) {
    bool hovered = (i == app.drop_chooser_hover);

    if (hovered) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.14);
      draw_rounded_rect(cr, cx + 5, ry + 2, w - 10, kChooserRowH - 4, 6);
      cairo_fill(cr);
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_text_extents_t rte;
    cairo_text_extents(cr, kDropActions[i], &rte);
    double rty = ry + (kChooserRowH - rte.height) / 2.0 - rte.y_bearing;
    cairo_move_to(cr, cx + 16, rty);
    cairo_show_text(cr, kDropActions[i]);

    ry += kChooserRowH;
  }
}

int hit_test_drop_chooser(const AppState& app, int x, int y) {
  if (!app.drop_chooser_open) return -1;
  const DropChooserGeometry g = drop_chooser_geometry(app);
  if (x < g.x || x >= g.x + g.w || y < g.y || y >= g.y + g.h)
    return -1;
  int rel_y = y - g.y;
  if (rel_y < kChooserHeaderH + kChooserSepH) return -1;
  int row = (rel_y - kChooserHeaderH - kChooserSepH) / kChooserRowH;
  if (row < 0 || row > 1) return -1;
  return row;
}

} // namespace eh::file_browser