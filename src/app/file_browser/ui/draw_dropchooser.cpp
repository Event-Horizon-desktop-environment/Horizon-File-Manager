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
#include "ui/design.hpp"
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

  draw_dialog_card(app, cr, cx, cy, w, kChooserH, 12);

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
  hui::design::hairline(cr, app, cx, ry, w);
  ry += 5;

  // Action rows: 0 = copy, 1 = move
  static const char* kDropActions[2] = {"Copy here", "Move here"};
  for (int i = 0; i < 2; ++i) {
    bool hovered = (i == app.drop_chooser_hover);
    app.hit_main.add(hui::Hit::menu(hui::Hit::kMenuDrop, i), cx, ry, w, kChooserRowH);

    if (hovered) {
      hui::design::row_hover(cr, app, cx + 6, ry + 2, w - 12, kChooserRowH - 4);
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
  // Resolved through the retained hit registry (row rects stored during
  // paint); the chooser geometry math is gone.
  if (!app.drop_chooser_open) return -1;
  const uint32_t hid = app.hit_main.query(x, y);
  if ((hid & hui::Hit::kGroupMask) != hui::Hit::kMenu ||
      hui::Hit::menu_id(hid) != hui::Hit::kMenuDrop)
    return -1;
  int row = hui::Hit::menu_row(hid);
  if (row < 0 || row > 1) return -1;
  return row;
}

} // namespace eh::file_browser