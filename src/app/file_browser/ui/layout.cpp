// layout.cpp — the ONE implementation of every grid/list geometry answer.
//
// Matches layout.hpp field-for-field. This is the code draw_grid_view used
// to hold privately; it now lives here EXACTLY (snap included) so draw,
// hit_test_grid, hit_test_list, and keyboard navigation all see the same
// numbers. The one previously-drifting constant — IconCache::bucket_down on
// icon_size — is applied in exactly this one line and nowhere else.
#include "layout.hpp"

#include "../app.hpp"

#include <algorithm>
#include <cmath>

#include <pango/pangocairo.h>

#include "platform/common/icon_cache/icon_cache.hpp"

namespace eh::file_browser {

// Real per-line height of the grid's 13px label font, measured ONCE through
// Pango (the label raster's own probe — a guess like 16px leaves the bottom
// of a wrapped 2-line name poking out under the selection/hover overlay).
// Cached so layout math never runs per-frame metric work.
static int measure_label_line_height_px() {
  cairo_surface_t* ps =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t* pc = cairo_create(ps);
  cairo_select_font_face(pc, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(pc, 13.0);
  PangoFontDescription* mdesc = pango_font_description_new();
  pango_font_description_set_family(mdesc, "Sans");
  pango_font_description_set_absolute_size(mdesc, 13 * PANGO_SCALE);
  PangoLayout* ppl = pango_cairo_create_layout(pc);
  PangoFontMetrics* mm = pango_context_get_metrics(
      pango_layout_get_context(ppl), mdesc, NULL);
  int lh = pango_font_metrics_get_height(mm) / PANGO_SCALE;
  pango_font_metrics_unref(mm);
  pango_font_description_free(mdesc);
  g_object_unref(ppl);
  cairo_destroy(pc);
  cairo_surface_destroy(ps);
  return lh < 1 ? 1 : lh;
}

PaneViewRect pane_view_rect_at(const AppState& app, int px) {
  int sidebar_w = app.sidebar_w();
  int info_w = 0;
  if (app.info_panel_open)
    info_w = std::max(200, static_cast<int>(280.0 * app.zoom_pct / 100.0));
  // Info panel must never squeeze the content column to nothing.
  if (info_w > 0) {
    int max_info = std::max(160, app.width - sidebar_w - 240);
    if (info_w > max_info) info_w = max_info;
  }
  int cx = sidebar_w;
  int cw = app.width - sidebar_w - info_w;
  int selector_h =
      (app.select_dir_mode || app.select_file_mode) ? app.select_bar_h : 0;
  bool banner_on = app.search_active || app.recursive_search_active ||
                   app.r_search_active || app.r_recursive_search_active;
  int cy = app.top_bar_height + app.tab_bar_height + (banner_on ? 28 : 0);
  int ch = app.height - cy - app.status_bar_height - selector_h;
  if (!app.split_view) return {cx, cy, std::max(0, cw), ch, 0};

  constexpr int kDivW = 4;
  int split =
      app.split_divider_x > 0 ? app.split_divider_x : std::max(200, cw) / 2;
  int left_w = std::max(100, split - kDivW / 2);
  int right_x = std::min(cx + cw - 100, cx + split + kDivW / 2);
  int right_w = std::max(100, cx + cw - right_x);
  int pt = app.top_bar_height;
  if (px >= right_x) return {right_x, cy + pt, right_w, ch - pt, 1};
  return {cx, cy + pt, left_w, ch - pt, 0};
}

PaneViewRect pane_view_rect_for_index(const AppState& app, int pane) {
  // Reuse the pixel picker with a probe that lands in the requested pane:
  // left/single under the sidebar, right past the divider.
  return pane_view_rect_at(app, pane == 1 ? app.width : app.sidebar_w());
}

GridLayout compute_grid_layout(int content_w, double zf) {
  GridLayout g;
  g.min_cell_w = static_cast<int>(110.0 * zf);
  g.col_gap = static_cast<int>(18.0 * zf);
  g.row_gap = static_cast<int>(10.0 * zf);
  g.cols = std::max(1, (content_w + g.col_gap) / (g.min_cell_w + g.col_gap));
  g.cell_w = (content_w - g.col_gap - (g.cols - 1) * g.col_gap) / g.cols;
  g.icon_size = eh::icons::IconCache::bucket_down(
      std::min(g.cell_w - static_cast<int>(16.0 * zf),
               static_cast<int>(72.0 * zf)));
  g.icon_area = g.icon_size;
  static const int base_line_h = measure_label_line_height_px();
  g.line_h = std::max(static_cast<int>(16.0 * zf),
                      static_cast<int>(std::lround(base_line_h * zf)));
  // 2 full text lines + pad so the label raster never exceeds the box and
  // the text gets a little breathing room (4px each side, centered) inside
  // the hover/selection overlay — a wrapped long name stays fully framed.
  g.label_h = 2 * g.line_h + 8;
  g.text_gap = static_cast<int>(4.0 * zf);
  g.item_h = g.icon_area + g.text_gap + g.label_h;
  g.row_h = g.item_h + g.row_gap;
  g.row_gap_at_top = g.row_gap;
  g.grid_w = g.cols * g.cell_w + (g.cols - 1) * g.col_gap;
  g.grid_offset_x = (content_w - g.grid_w) / 2;
  return g;
}

ListLayout compute_list_layout(double zf) {
  ListLayout l;
  l.entry_h = std::max(20, static_cast<int>(std::lround(36.0 * zf)));
  l.col_header_h = static_cast<int>(l.entry_h * 0.55);
  return l;
}

// Tab whose entry list lives in the pane under pixel px (see layout.hpp).
Tab& pane_tab_at(AppState& app, int px) {
  if (app.split_view && pane_view_rect_at(app, px).pane == 1)
    return app.right_pane;
  return app.tabs[app.active_tab];
}

} // namespace eh::file_browser
