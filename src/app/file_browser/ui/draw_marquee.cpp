#include "../app.hpp"
#include "../trace.hpp"
#include "../features/sidebar.hpp"
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

#include "draw_helpers.hpp"
#include "draw_file_icons.hpp"
#include "draw_thumbnails.hpp"
#include "layout.hpp"

#include "platform/common/icon_cache/icon_cache.hpp"
#include "app/file_browser/features/svg_preview.hpp"
#include "app/file_browser/features/video_preview.hpp"
#include "app/file_browser/features/pdf_preview.hpp"
#include "app/file_browser/features/epub_preview.hpp"
#include "app/file_browser/features/image_preview.hpp"
#include "app/file_browser/features/thumbnail_cache.hpp"
#include "app/file_browser/ui/pixblit.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {

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

// ── Shared split-view geometry ───────────────────────────────────
// Single source of truth for hit-testing. pane_view_rect_at and
// pane_tab_at live in layout.cpp / layout.hpp.
void hit_test_marquee(AppState& app) {
  double x0 = std::min(app.marquee_x0, app.marquee_x1);
  double y0 = std::min(app.marquee_y0, app.marquee_y1);
  double x1 = std::max(app.marquee_x0, app.marquee_x1);
  double y1 = std::max(app.marquee_y0, app.marquee_y1);

  // Resolve pane from the marquee's horizontal midpoint.
  int mcx = static_cast<int>((x0 + x1) / 2.0);
  PaneViewRect r = pane_view_rect_at(app, mcx);
  Tab& tab = pane_tab_at(app, mcx);

  int content_x = r.x;
  int content_w = r.w;
  int content_y = r.y;

  tab.multi_selected.clear();
  tab.selected_idx = -1;

  if (tab.view_mode == ViewMode::List) {
    int entry_h = app.entry_height;
    int header_h = static_cast<int>(entry_h * 0.55);
    bool grouped = tab.group_by_type;
    int prev_type = -1;
    int acc = 0;
    for (int i = 0; i < static_cast<int>(tab.visible_entries.size()); ++i) {
      if (grouped) {
        int rr = tab.visible_entries[i];
        if (rr >= 0 && rr < static_cast<int>(tab.entries.size())) {
          int t = static_cast<int>(tab.entries[rr].type);
          if (t != prev_type) { acc += header_h; prev_type = t; }
        }
      }
      double iy = static_cast<double>(content_y - tab.scroll_px + acc);
      double ih = static_cast<double>(entry_h);
      if (iy > y1) break;
      if (iy + ih >= y0 && !(x1 < content_x || x0 > content_x + content_w)) {
        tab.multi_selected.push_back(i);
        if (tab.selected_idx < 0) tab.selected_idx = i;
      }
      acc += entry_h;
    }
  } else if (tab.view_mode == ViewMode::Compact) {
    double zf = app.zoom_pct / 100.0;
    int entry_h = static_cast<int>(24.0 * zf);
    for (int i = 0; i < static_cast<int>(tab.visible_entries.size()); ++i) {
      double iy = static_cast<double>(content_y - tab.scroll_px + i * entry_h);
      double ih = static_cast<double>(entry_h);
      if (iy + ih < y0) continue;
      if (iy > y1) break;
      if (x1 < content_x || x0 > content_x + content_w) continue;
      tab.multi_selected.push_back(i);
      if (tab.selected_idx < 0) tab.selected_idx = i;
    }
  } else if (tab.view_mode == ViewMode::Grid) {
    // Recompute layout from THIS pane's width — the app.grid_* globals are
    // overwritten by whichever pane drew last and go stale in split view.
    double zf = app.zoom_pct / 100.0;
    int min_cell_w = static_cast<int>(110.0 * zf);
    int col_gap = static_cast<int>(18.0 * zf);
    int row_gap = static_cast<int>(10.0 * zf);
    int cols = std::max(1, (content_w + col_gap) / (min_cell_w + col_gap));
    int cell_w = (content_w - col_gap - (cols - 1) * col_gap) / cols;

    // Must mirror draw_grid_view exactly: icon_size, item_h, and the
    // horizontal centering offset all factor into cell placement.
    int icon_size = std::min(cell_w - static_cast<int>(16.0 * zf),
                             static_cast<int>(72.0 * zf));
    int text_gap = static_cast<int>(4.0 * zf);
    int label_h = static_cast<int>(32.0 * zf); // 2 lines of label text
    int item_h = icon_size + text_gap + label_h;
    int row_h = item_h + row_gap;

    int grid_w = cols * cell_w + (cols - 1) * col_gap;
    int grid_offset_x = (content_w - grid_w) / 2;

    int gy = content_y + row_gap - tab.scroll_px;

    double clamp_x1 = std::min(x1, static_cast<double>(content_x + content_w));
    for (int i = 0; i < static_cast<int>(tab.visible_entries.size()); ++i) {
      int col = i % cols;
      int row = i / cols;
      double gx = static_cast<double>(content_x + grid_offset_x + col * (cell_w + col_gap));
      double gyy = static_cast<double>(gy + row * row_h);
      double gcw = static_cast<double>(cell_w);
      double gch = static_cast<double>(item_h);
      if (gyy + gch < y0) continue;
      if (gyy > y1) break;
      if (gx + gcw < x0 || gx > clamp_x1) continue;
      tab.multi_selected.push_back(i);
      if (tab.selected_idx < 0) tab.selected_idx = i;
    }
  }
}

} // namespace eh::file_browser