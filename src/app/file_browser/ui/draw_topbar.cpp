// draw_topbar.cpp — house icon, path-bar pill and the full top-bar composite.
// Moved wholesale from ui/draw.cpp (byte-identical bodies).

#include "../app.hpp"
#include "../trace.hpp"
#include "../features/sidebar/sidebar.hpp"
#include "../features/view_zoom/view_zoom.hpp"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

#include "platform/common/icon_cache/icon_cache.hpp"

#include "ui/design.hpp"
#include "ui/hit.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {

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

// Path-bar glassy pill: 5 cairo passes with vertical gradients over a
// ~pane-wide rounded rect. Appearance depends only on geometry + theme, so
// it's preredered to a cache surface (per pane slot) and blitted each frame.
static void draw_pill_into(cairo_t* cr, const AppState& app,
                           int px, int py, int pw, int ph) {
  // Full-stadium glass pill (M3 SearchBar shape) with the original
  // translucent Tahoe-style glass rendering.
  int bar_radius = ph / 2;

  // Base fill: dark translucent, high see-through so the inner glass shows
  cairo_pattern_t* grad = cairo_pattern_create_linear(0, py, 0, py + ph);
  cairo_pattern_add_color_stop_rgba(grad, 0.0, app.surface_r, app.surface_g, app.surface_b, 0.30);
  cairo_pattern_add_color_stop_rgba(grad, 1.0, app.bg_r, app.bg_g, app.bg_b, 0.30);
  cairo_set_source(cr, grad);
  draw_rounded_rect(cr, px, py, pw, ph, bar_radius);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  // Very subtle outer separation only (no strong outer halo/glow — glassy part is inner)
  {
    cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
    cairo_set_line_width(cr, 1.0);
    draw_rounded_rect(cr, px + 0.5, py + 0.5, pw - 1.0, ph - 1.0,
                      static_cast<double>(bar_radius));
    cairo_stroke(cr);
  }

  // Inner semi-glassy bright rim — clean inner glassy frame (no bevel at all).
  {
    cairo_pattern_t* rim = cairo_pattern_create_linear(0, py, 0, py + ph);
    cairo_pattern_add_color_stop_rgba(rim, 0.00, app.text_r, app.text_g, app.text_b, 0.28);
    cairo_pattern_add_color_stop_rgba(rim, 0.18, app.text_r, app.text_g, app.text_b, 0.14);
    cairo_pattern_add_color_stop_rgba(rim, 0.42, app.text_r, app.text_g, app.text_b, 0.03);
    cairo_pattern_add_color_stop_rgba(rim, 1.00, app.text_r, app.text_g, app.text_b, 0.09);
    cairo_set_source(cr, rim);
    cairo_set_line_width(cr, 1.35);
    double inner_inset = 2.8;
    draw_rounded_rect(cr,
                      px + inner_inset,
                      py + inner_inset,
                      pw - inner_inset * 2,
                      ph - inner_inset * 2,
                      static_cast<double>(bar_radius) - inner_inset + 0.5);
    cairo_stroke(cr);
    cairo_pattern_destroy(rim);
  }

  // Extra top-inner highlight band (pure glass catch)
  {
    cairo_pattern_t* top_hl = cairo_pattern_create_linear(0, py + 2, 0, py + ph * 0.4);
    cairo_pattern_add_color_stop_rgba(top_hl, 0.0, app.text_r, app.text_g, app.text_b, 0.09);
    cairo_pattern_add_color_stop_rgba(top_hl, 1.0, app.text_r, app.text_g, app.text_b, 0.0);
    cairo_set_source(cr, top_hl);
    cairo_set_line_width(cr, 0.7);
    double hl_inset = 3.8;
    draw_rounded_rect(cr,
                      px + hl_inset,
                      py + hl_inset,
                      pw - hl_inset * 2,
                      ph - hl_inset * 2,
                      static_cast<double>(bar_radius) - hl_inset + 1);
    cairo_stroke(cr);
    cairo_pattern_destroy(top_hl);
  }

  // Bottom-inner highlight band (symmetric to top, softer)
  {
    cairo_pattern_t* bottom_hl = cairo_pattern_create_linear(0, py + ph * 0.55, 0, py + ph - 2);
    cairo_pattern_add_color_stop_rgba(bottom_hl, 0.0, app.text_r, app.text_g, app.text_b, 0.0);
    cairo_pattern_add_color_stop_rgba(bottom_hl, 1.0, app.text_r, app.text_g, app.text_b, 0.07);
    cairo_set_source(cr, bottom_hl);
    cairo_set_line_width(cr, 0.7);
    double hl_inset = 3.8;
    draw_rounded_rect(cr,
                      px + hl_inset,
                      py + hl_inset,
                      pw - hl_inset * 2,
                      ph - hl_inset * 2,
                      static_cast<double>(bar_radius) - hl_inset + 1);
    cairo_stroke(cr);
    cairo_pattern_destroy(bottom_hl);
  }
}

static void draw_top_bar_pill(AppState& app, cairo_t* cr,
                              int px, int py, int pw, int ph, int slot) {
  if (pw <= 0 || ph <= 0) return;

  std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
  auto mix = [&seed](double v) {
    std::uint64_t b;
    std::memcpy(&b, &v, sizeof b);
    seed ^= b + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
  };
  mix(app.surface_r); mix(app.surface_g); mix(app.surface_b);
  mix(app.bg_r); mix(app.bg_g); mix(app.bg_b);
  mix(app.text_r); mix(app.text_g); mix(app.text_b);
  seed ^= (std::uint64_t)(std::uint32_t)px;
  seed ^= (std::uint64_t)(std::uint32_t)py + 0x9e3779b97f4a7c15ULL;
  seed ^= (std::uint64_t)(std::uint32_t)pw;
  seed ^= (std::uint64_t)(std::uint32_t)ph;
  seed ^= (std::uint64_t)(std::uint32_t)(ph / 2);
  seed ^= 0x4D334D5458544152ULL; // M3 stadium-pill style version

  auto blit = [&](const AppState::PillCache& pc) {
    cairo_save(cr);
    cairo_rectangle(cr, px, py, pw, ph);
    cairo_clip(cr);
    cairo_set_source_surface(cr, pc.surface, (double)px, (double)py);
    cairo_paint(cr);
    cairo_restore(cr);
  };

  // Cache off (validation / A-B): render exactly like the original inline code.
  if (std::getenv("HORIZON_PILL_CACHE") != nullptr) {
    draw_pill_into(cr, app, px, py, pw, ph);
    return;
  }

  AppState::PillCache& pc = app.pill_cache[static_cast<unsigned>(slot) % 4];
  if (pc.surface && pc.w == pw && pc.h == ph && pc.key == seed) {
    blit(pc);
    return;
  }

  if (pc.surface &&
      (cairo_image_surface_get_width(pc.surface) != pw ||
       cairo_image_surface_get_height(pc.surface) != ph)) {
    cairo_surface_destroy(pc.surface);
    pc.surface = nullptr;
  }
  if (!pc.surface)
    pc.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
  cairo_t* ic = cairo_create(pc.surface);
  cairo_translate(ic, (double)-px, (double)-py);
  draw_pill_into(ic, app, px, py, pw, ph);
  cairo_destroy(ic);
  cairo_surface_flush(pc.surface);
  pc.w = pw;
  pc.h = ph;
  pc.key = seed;
  blit(pc);
}

void draw_top_bar(AppState& app, cairo_t* cr, int w, int top_h, int y0, int pane_x, int pane_w) {
  double zf = app.zoom_pct / 100.0;

  int sidebar_w;
  int content_right;
  if (pane_w > 0) {
    sidebar_w = pane_x;
    content_right = pane_x + pane_w;
  } else {
    sidebar_w = app.sidebar_w();
    content_right = w;
  }

  // Window controls position (traffic lights always on the right)
  if (pane_w == 0) {
    app.win_btn_x = 0;
    app.win_btn_w = static_cast<int>(16.0 * zf);
  }

  // In split view, the global bar only draws window controls (per-pane bars draw everything else)
  if (!app.split_view || pane_w > 0) {

  // ── Responsive visibility ──
  // Lowest-priority controls hide first so the path bar never collapses
  // into overlap. Back + path + traffic always stay; everything hidden
  // remains reachable via keyboard shortcuts.
  int avail_w = content_right - sidebar_w;
  int arrow_slot = static_cast<int>(36.0 * zf);
  int gap = static_cast<int>(6.0 * zf); // 6px header-bar spacing
  int left_pad = static_cast<int>(20.0 * zf); // px-5
  int path_margin = static_cast<int>(24.0 * zf); // mx-6
  int right_margin = static_cast<int>(16.0 * zf);
  int gear_gap = static_cast<int>(8.0 * zf);
  int view_toggle_w = static_cast<int>(40.0 * zf);
  int sort_w = static_cast<int>(28.0 * zf);
  int gear_w = static_cast<int>(36.0 * zf);
  int traffic_w = static_cast<int>(52.0 * zf);
  int folder_search_btn_w = static_cast<int>(40.0 * zf);
  int search_btn_w = static_cast<int>(40.0 * zf);
  int min_path_w = std::max(80, static_cast<int>(120.0 * zf));
  int fold_extra = (app.sidebar_folded && pane_w == 0) ? arrow_slot + gap : 0;

  bool show_forward = true, show_up = true;
  bool show_folder = true, show_search = true;
  bool show_sort = true, show_view = true, show_gear = true;

  auto path_spare_for = [&](bool fwd, bool up, bool folder, bool search, bool sort, bool view,
                            bool gear) -> int {
    int left_used = left_pad + fold_extra + (arrow_slot + gap) + (fwd ? arrow_slot + gap : 0) +
                    (up ? arrow_slot + gap : 0);
    int n = 0, sum = 0;
    if (folder) { sum += folder_search_btn_w; ++n; }
    if (search) { sum += search_btn_w; ++n; }
    if (view) { sum += view_toggle_w; ++n; }
    if (sort) { sum += sort_w; ++n; }
    if (gear) { sum += gear_w; ++n; }
    int gaps = (n > 0) ? (n - 1) * gap : 0;
    if (view && sort) gaps -= gap; // flush compound control
    if (pane_w == 0) {
      sum += traffic_w;
      gaps += (gear || n > 0) ? ((gear) ? gear_gap : gap) : 0;
    }
    int required = left_used + path_margin + min_path_w + gap + sum + gaps + right_margin;
    return avail_w - required; // >= 0 fits
  };

  // Narrow-window breakpoints: below ~700px of header width the toolbar
  // groups leave the header while the path bar stays put; further groups
  // collapse as it narrows further. Measured on the header's own width
  // (avail_w, per pane in split view): the header shares the window with
  // the sidebar, so window width would under-collapse and squeeze the
  // pill. The fit loop below stays as a safety net so the bar can never
  // overlap.
  int bp_w = (pane_w > 0) ? pane_w : avail_w;
  int bp1 = static_cast<int>(700.0 * zf);
  int bp2 = static_cast<int>(500.0 * zf);
  int bp3 = static_cast<int>(400.0 * zf);
  if (bp_w < bp1) {
    show_forward = false;
    show_up = false;
    show_folder = false;
    show_sort = false;
    show_gear = false;
  }
  if (bp_w < bp2) show_search = false;
  if (bp_w < bp3) show_view = false;

  // Safety net: never overlap. Hide order is least-essential first
  // (forward, up, folder dup, sort, gear, search, view last).
  for (int step = 0; step < 7; ++step) {
    if (path_spare_for(show_forward, show_up, show_folder, show_search, show_sort, show_view,
                       show_gear) >= 0)
      break;
    switch (step) {
      case 0: show_forward = false; break;
      case 1: show_up = false; break;
      case 2: show_folder = false; break;
      case 3: show_sort = false; break;
      case 4: show_gear = false; break;
      case 5: show_search = false; break;
      default: show_view = false; break;
    }
  }

  // ── Navigation arrows (back, forward) ──
  int x = sidebar_w + static_cast<int>(20.0 * zf); // px-5

  // ── Sidebar fold toggle button ──
  // Appears at the far left when the sidebar is folded into a flap overlay.
  if (app.sidebar_folded && pane_w == 0) {
    int slot_w = static_cast<int>(36.0 * zf);
    bool t_hover = app.sidebar_toggle_hover;
    bool t_active = app.sidebar_folded_revealed;
    app.sidebar_toggle_x = x;
    app.sidebar_toggle_w = slot_w;
    app.hit_main.add(hui::Hit::topbar(app.active_pane, hui::Hit::kTopFoldToggle), x, y0 + (top_h - slot_w) / 2,
                     slot_w, slot_w);
    if (t_hover || t_active) {
      cairo_save(cr);
      if (t_active) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      } else {
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      }
      draw_rounded_rect(cr, x, (top_h - slot_w) / 2, slot_w, slot_w,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    if (app.sidebar_toggle_svg) {
      double svg_w = static_cast<double>(
          cairo_image_surface_get_width(app.sidebar_toggle_svg));
      double svg_h = static_cast<double>(
          cairo_image_surface_get_height(app.sidebar_toggle_svg));
      int sz = static_cast<int>(18.0 * zf);
      int ox = x + (slot_w - sz) / 2;
      int oy = (top_h - sz) / 2;
      double display_scale = sz / std::max(svg_w, svg_h);
      cairo_save(cr);
      if (t_active) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
      } else {
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      }
      cairo_rectangle(cr, ox, oy, sz, sz);
      cairo_clip(cr);
      cairo_translate(cr, ox, oy);
      cairo_scale(cr, display_scale, display_scale);
      cairo_mask_surface(cr, app.sidebar_toggle_svg, 0, 0);
      cairo_restore(cr);
    } else {
      // fallback: three-block "panel" glyph mirroring layout.svg
      int sz = static_cast<int>(18.0 * zf);
      int oy = (top_h - sz) / 2;
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_set_line_width(cr, 1.4 * zf);
      int gap = static_cast<int>(3.0 * zf);
      for (int i = 0; i < 3; ++i) {
        double yy = oy + i * (sz / 3.0) + gap;
        cairo_move_to(cr, x + 9.0 * zf, yy);
        cairo_line_to(cr, x + slot_w - 9.0 * zf, yy);
        cairo_stroke(cr);
      }
    }
    x += slot_w + static_cast<int>(6.0 * zf);
  } else {
    app.sidebar_toggle_x = 0;
    app.sidebar_toggle_w = 0;
  }

  auto draw_arrow = [&](int idx, cairo_surface_t* svg, const char* fallback,
                        bool hovered, bool enabled) {
    int slot_w = static_cast<int>(36.0 * zf);
    double alpha = enabled ? 1.0 : 0.38;
    if (hovered && enabled) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, x, (top_h - slot_w) / 2, slot_w, slot_w,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, alpha);
    if (svg) {
      double svg_w = static_cast<double>(cairo_image_surface_get_width(svg));
      double svg_h = static_cast<double>(cairo_image_surface_get_height(svg));
      int sz = static_cast<int>(18.0 * zf);
      int ox = x + (slot_w - sz) / 2;
      int oy = (top_h - sz) / 2;
      double display_scale = sz / std::max(svg_w, svg_h);
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, alpha);
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
    // Store position for hit testing + register the retained region.
    if (idx == 0) (app.active_pane ? app.r_arrow_back_x : app.arrow_back_x) = x;
    else if (idx == 1) (app.active_pane ? app.r_arrow_forward_x : app.arrow_forward_x) = x;
    else (app.active_pane ? app.r_arrow_up_x : app.arrow_up_x) = x;
    const int nav_ctrl = (idx == 0) ? hui::Hit::kTopNavBack : (idx == 1) ? hui::Hit::kTopNavForward : hui::Hit::kTopNavUp;
    app.hit_main.add(hui::Hit::topbar(app.active_pane, nav_ctrl), x, y0 + (top_h - slot_w) / 2, slot_w,
                     slot_w);
    x += slot_w + static_cast<int>(6.0 * zf); // 6px header-bar spacing
  };

  draw_arrow(0, app.arrow_left_svg, "<",
             app.active_pane ? app.r_arrow_back_hover : app.arrow_back_hover, true);
  if (show_forward) {
    draw_arrow(1, app.arrow_right_svg, ">",
               app.active_pane ? app.r_arrow_forward_hover : app.arrow_forward_hover, true);
  } else {
    (app.active_pane ? app.r_arrow_forward_x : app.arrow_forward_x) = -1;
  }
  if (show_up) {
    draw_arrow(2, app.arrow_up_svg, "^",
               app.active_pane ? app.r_arrow_up_hover : app.arrow_up_hover,
               can_navigate_up(app));
  } else {
    (app.active_pane ? app.r_arrow_up_x : app.arrow_up_x) = -1;
  }

  int path_left = x;

  // ── Right-side controls ──
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * zf);

  // Layout from right edge, skipping hidden controls (see hide loop above).
  int cursor = content_right - right_margin;
  int traffic_rx = -1;
  if (pane_w == 0) {
    traffic_rx = cursor - traffic_w;
    cursor = traffic_rx - gear_gap;
  }
  int gear_x = -1;
  if (show_gear) {
    gear_x = cursor - gear_w;
    cursor = gear_x - gap;
  } else if (pane_w == 0) {
    cursor += gear_gap - gap;
  }
  int sort_x = -1, view_toggle_x = -1;
  if (show_sort && show_view) {
    sort_x = cursor - sort_w;
    view_toggle_x = sort_x - view_toggle_w;
    cursor = view_toggle_x - gap;
  } else if (show_view) {
    view_toggle_x = cursor - view_toggle_w;
    cursor = view_toggle_x - gap;
  } else if (show_sort) {
    sort_x = cursor - sort_w;
    cursor = sort_x - gap;
  }
  int search_btn_x = -1;
  if (show_search) {
    search_btn_x = cursor - search_btn_w;
    cursor = search_btn_x - gap;
  }
  int folder_search_btn_x = -1;
  if (show_folder) {
    folder_search_btn_x = cursor - folder_search_btn_w;
    cursor = folder_search_btn_x - gap;
  }

  (app.active_pane ? app.r_search_btn_x : app.search_btn_x) = show_search ? search_btn_x : 0;
  (app.active_pane ? app.r_search_btn_w : app.search_btn_w) = show_search ? search_btn_w : 0;
  (app.active_pane ? app.r_folder_search_btn_x : app.folder_search_btn_x) =
      show_folder ? folder_search_btn_x : 0;
  (app.active_pane ? app.r_folder_search_btn_w : app.folder_search_btn_w) =
      show_folder ? folder_search_btn_w : 0;
  (app.active_pane ? app.r_view_btn_x : app.view_btn_x) = show_view ? view_toggle_x : 0;
  (app.active_pane ? app.r_view_btn_w : app.view_btn_w) = show_view ? view_toggle_w : 0;
  (app.active_pane ? app.r_sort_btn_x : app.sort_btn_x) = show_sort ? sort_x : 0;
  (app.active_pane ? app.r_sort_btn_w : app.sort_btn_w) = show_sort ? sort_w : 0;

  // Path bar fills remaining space down to the last-resort floor.
  int leftmost_right;
  if (show_folder) leftmost_right = folder_search_btn_x;
  else if (show_search) leftmost_right = search_btn_x;
  else if (show_view) leftmost_right = view_toggle_x;
  else if (show_sort) leftmost_right = sort_x;
  else if (show_gear) leftmost_right = gear_x;
  else leftmost_right = (pane_w == 0) ? traffic_rx : content_right - right_margin;
  int path_x = path_left + path_margin;
  int path_w = leftmost_right - gap - path_x;
  if (path_w < 60) path_w = 60;

  int path_h = top_h - static_cast<int>(16.0 * zf);
  int path_y = (top_h - path_h) / 2;

  // ── Compound "View Options" control (split-button style) ──
  // View toggle + sort chevron share one linked pill (rounded outer corners,
  // square inner edge) with a 1px divider between the two halves.
  // Hidden halves collapse: a lone survivor renders as a standalone button.
  if (show_view || show_sort) {
    int combo_x = show_view ? view_toggle_x : sort_x;
    int combo_w = (show_view && show_sort)
                      ? sort_x + sort_w - view_toggle_x
                      : (show_view ? view_toggle_w : sort_w);
    bool vhv = show_view &&
               (app.active_pane ? app.r_view_mode_btn_hover : app.view_mode_btn_hover);
    bool sort_hv = show_sort &&
                   (app.active_pane ? app.r_sort_btn_hover : app.sort_btn_hover);
    bool sort_active = show_sort &&
                       (app.active_pane ? app.r_sort_menu_open : app.sort_menu_open);
    if (vhv || sort_hv || sort_active) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                            sort_active ? 0.14 : 0.08);
      draw_rounded_rect(cr, combo_x, path_y, combo_w, path_h,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    if (show_view && show_sort) {
      double div_x = sort_x + 0.5;
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.14);
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, div_x, path_y + static_cast<int>(5.0 * zf));
      cairo_line_to(cr, div_x, path_y + path_h - static_cast<int>(5.0 * zf));
      cairo_stroke(cr);
    }
  }

  // ── Gradient bar background + semi-glassy design (inner glassy rim, not outer)
  draw_top_bar_pill(app, cr, path_x, path_y, path_w, path_h,
                    (pane_w > 0) ? (app.active_pane ? 2 : 1) : 0);

  // M3 focus ring: 2px accent outline while searching or editing the path.
  if ((app.active_pane ? app.r_search_active : app.search_active) ||
      (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active) ||
      (app.active_pane ? app.r_path_editing : app.path_editing)) {
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.9);
    cairo_set_line_width(cr, 1.5);
    draw_rounded_rect(cr, path_x, path_y, path_w, path_h, path_h / 2.0);
    cairo_stroke(cr);
  }

  // ── Three-dot menu on the far right ──
  int dots_btn_w = static_cast<int>(24.0 * zf);
  int dots_x = path_x + path_w - dots_btn_w + static_cast<int>(2.0 * zf);
  int dots_y = path_y;
  (app.active_pane ? app.r_dots_btn_x : app.dots_btn_x) = dots_x;
  (app.active_pane ? app.r_dots_btn_y : app.dots_btn_y) = dots_y;
  (app.active_pane ? app.r_dots_btn_w : app.dots_btn_w) = dots_btn_w;
  (app.active_pane ? app.r_dots_btn_h : app.dots_btn_h) = path_h;
  // Three dots (⋮) — bold SVG, circle fallback
  {
    bool dhover = (app.active_pane ? app.r_dots_btn_hover : app.dots_btn_hover);
    if (dhover) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, dots_x - 2, path_y + 2, dots_btn_w + 4, path_h - 4,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    if (app.three_dots_svg) {
      int dsz = static_cast<int>(14.0 * zf);
      int dox = dots_x + (dots_btn_w - dsz) / 2;
      int doy = path_y + (path_h - dsz) / 2;
      double svg_w = static_cast<double>(cairo_image_surface_get_width(app.three_dots_svg));
      double svg_h = static_cast<double>(cairo_image_surface_get_height(app.three_dots_svg));
      double display_scale = dsz / std::max(svg_w, svg_h);
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                            dhover ? 0.95 : 0.65);
      cairo_rectangle(cr, dox, doy, dsz, dsz);
      cairo_clip(cr);
      cairo_translate(cr, dox, doy);
      cairo_scale(cr, display_scale, display_scale);
      cairo_mask_surface(cr, app.three_dots_svg, 0, 0);
      cairo_restore(cr);
    } else {
      double dot_r = 1.1 * zf;
      double gap = 2.8 * zf;
      double cx = dots_x + dots_btn_w / 2.0;
      double cy0 = path_y + path_h / 2.0 - gap - dot_r;
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                              dhover ? 0.85 : 0.5);
      for (int i = 0; i < 3; ++i) {
        cairo_arc(cr, cx, cy0 + i * (2.0 * dot_r + gap), dot_r, 0.0, 2.0 * M_PI);
        cairo_fill(cr);
      }
    }
  }

  // ── Path content (house icon + breadcrumbs or editing) ──
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * zf);

  int text_x = path_x + static_cast<int>(12.0 * zf);
  int text_y = top_h / 2 + static_cast<int>(4.0 * zf);

  // Draw location icon (home / music / video / documents — bold SVG,
  // vector house fallback). Sidebar icons are untouched.
  int icon_sz = static_cast<int>(12.0 * zf);
  int icon_y = (top_h - icon_sz) / 2;
  {
    cairo_surface_t* nav_svg = app.home_nav_svg;
    std::string hp = home_dir();
    const std::string& cp = app.cur_tab().current_path;
    if (cp == hp + "/Music")           nav_svg = app.music_nav_svg;
    else if (cp == hp + "/Videos")     nav_svg = app.video_nav_svg;
    else if (cp == hp + "/Documents")  nav_svg = app.documents_nav_svg;
    if (nav_svg) {
      double svg_w = static_cast<double>(cairo_image_surface_get_width(nav_svg));
      double svg_h = static_cast<double>(cairo_image_surface_get_height(nav_svg));
      double display_scale = icon_sz / std::max(svg_w, svg_h);
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.85);
      cairo_rectangle(cr, text_x, icon_y, icon_sz, icon_sz);
      cairo_clip(cr);
      cairo_translate(cr, text_x, icon_y);
      cairo_scale(cr, display_scale, display_scale);
      cairo_mask_surface(cr, nav_svg, 0, 0);
      cairo_restore(cr);
    } else {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.75);
      draw_house_icon(cr, text_x, icon_y, icon_sz);
    }
  }

  int path_text_x = text_x + icon_sz + static_cast<int>(12.0 * zf); // gap-3
  int path_text_w = path_w - (path_text_x - path_x) - dots_btn_w - static_cast<int>(16.0 * zf);
  app.hit_main.add(hui::Hit::topbar(app.active_pane, hui::Hit::kTopPathBar), path_x, y0, path_w,
                   top_h);
  app.hit_main.add(hui::Hit::topbar(app.active_pane, hui::Hit::kTopPathText), path_text_x,
                   y0 + path_y, path_text_w, path_h);

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
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
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
      int btn_r = static_cast<int>(8.0 * zf);

      // Active filter chip: M3 secondary-container treatment
      if (any_active) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      } else {
        hui::design::card_fill(cr, app, hv ? 0.75 : 0.55);
      }
      draw_rounded_rect(cr, right_cursor, by, bw, bh, btn_r);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                            any_active ? 0.55 : hv ? 0.35 : 0.22);
      cairo_set_line_width(cr, 1.0);
      draw_rounded_rect(cr, right_cursor + 0.5, by + 0.5, bw - 1.0, bh - 1.0, btn_r);
      cairo_stroke(cr);

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
            // M3 active indicator: accent pill, accent-tinted label
            cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
            draw_rounded_rect(cr, sx + 1, by2, seg_w - 2, bh2, 5);
            cairo_fill(cr);
          } else if (hv2 && si == hov_btn) {
            cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
            draw_rounded_rect(cr, sx + 1, by2, seg_w - 2, bh2, 5);
            cairo_fill(cr);
          }
          cairo_text_extents_t te2;
          cairo_text_extents(cr, kSegLabels[si], &te2);
          if (sel) {
            cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
          } else {
            cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.66);
          }
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
    std::string display_label = hui::design::clip_end(cr, label, path_text_w - 4);
    cairo_text_extents_t label_te;
    cairo_text_extents(cr, display_label.c_str(), &label_te);
    int label_w = static_cast<int>(label_te.x_advance + 4.0 * zf);
    if (label_w > path_text_w) label_w = path_text_w;

    bool label_hovered = ((app.active_pane ? app.r_breadcrumb_hover : app.breadcrumb_hover) == 0);
    if (label_hovered) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, path_text_x - 2, path_y + 2, label_w + 4, path_h - 4,
                        static_cast<int>(8.0 * zf));
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
  if (show_view) {
    // Icon previews the mode the next click switches TO.
    cairo_surface_t* svg = nullptr;
    const char* fallback = "\u25A6";
    switch (app.cur_tab().view_mode) {
      case ViewMode::List:    svg = app.view_grid_svg;    fallback = "\u25A6"; break; // next: Grid
      case ViewMode::Grid:    svg = app.view_compact_svg; fallback = "\u2261"; break; // next: Compact
      case ViewMode::Compact: svg = app.view_tree_svg;    fallback = "\u25B3"; break; // next: Tree
      case ViewMode::Tree:    svg = app.view_list_svg;    fallback = "\u25A3"; break; // next: List
      default:                svg = app.view_grid_svg;    fallback = "\u25A6"; break;
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    int sz = static_cast<int>(16.0 * zf);
    int ox = view_toggle_x + (view_toggle_w - sz) / 2;
    int oy = (top_h - sz) / 2;
    if (svg) {
      double svg_w = static_cast<double>(cairo_image_surface_get_width(svg));
      double svg_h = static_cast<double>(cairo_image_surface_get_height(svg));
      double display_scale = sz / std::max(svg_w, svg_h);
      cairo_save(cr);
      cairo_rectangle(cr, ox, oy, sz, sz);
      cairo_clip(cr);
      cairo_translate(cr, ox, oy);
      cairo_scale(cr, display_scale, display_scale);
      cairo_mask_surface(cr, svg, 0, 0);
      cairo_restore(cr);
    } else {
      cairo_text_extents_t te;
      cairo_text_extents(cr, fallback, &te);
      cairo_move_to(cr, ox + (sz - te.width) / 2, oy + sz / 2 + te.height / 2);
      cairo_show_text(cr, fallback);
    }
  }

  // ── Folder-search button (folder + magnifying glass) ──
  if (show_folder) {
    bool hv = app.active_pane ? app.r_folder_search_btn_hover : app.folder_search_btn_hover;
    bool active = (app.active_pane ? app.r_search_active : app.search_active);
    if (hv || active) {
      cairo_save(cr);
      if (active) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      } else {
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      }
      draw_rounded_rect(cr, folder_search_btn_x, path_y, folder_search_btn_w, path_h,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    if (active) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
    } else {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    }
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
  if (show_search) {
    bool hv = app.active_pane ? app.r_search_btn_hover : app.search_btn_hover;
    bool active = (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active);
    if (hv || active) {
      cairo_save(cr);
      if (active) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      } else {
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      }
      draw_rounded_rect(cr, search_btn_x, path_y, search_btn_w, path_h,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    if (active) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
    } else {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    }
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

  // ── Sort chevron (dropdown segment of the compound View Options control) ──
  if (show_sort) {
    int csz = static_cast<int>(18.0 * zf); // same as the other toolbar icons
    int cx = sort_x + (sort_w - csz) / 2;
    int cy = (top_h - csz) / 2;
    if (app.sort_chevron_svg) {
      double csvg_w = static_cast<double>(cairo_image_surface_get_width(app.sort_chevron_svg));
      double csvg_h = static_cast<double>(cairo_image_surface_get_height(app.sort_chevron_svg));
      double display_scale = csz / std::max(csvg_w, csvg_h);
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_rectangle(cr, cx, cy, csz, csz);
      cairo_clip(cr);
      cairo_translate(cr, cx, cy);
      cairo_scale(cr, display_scale, display_scale);
      cairo_mask_surface(cr, app.sort_chevron_svg, 0, 0);
      cairo_restore(cr);
    } else {
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12.0 * zf);
      cairo_move_to(cr, cx, top_h / 2 + static_cast<int>(4.0 * zf));
      cairo_show_text(cr, "\u25bc");
    }
  }

  // ── Settings gear button ──
  if (show_gear) {
    bool hv = app.active_pane ? app.r_settings_btn_hover : app.settings_btn_hover;
    app.hit_main.add(hui::Hit::topbar(app.active_pane, hui::Hit::kTopGear), gear_x, y0 + path_y, gear_w,
                     path_h);
    if (hv) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, gear_x, path_y, gear_w, path_h,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      cairo_restore(cr);
    }
    int gz = static_cast<int>(15.0 * zf);
    int gox = gear_x + (gear_w - gz) / 2;
    int goy = (top_h - gz) / 2;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    if (app.settings_gear_svg) {
      double svg_w = static_cast<double>(cairo_image_surface_get_width(app.settings_gear_svg));
      double svg_h = static_cast<double>(cairo_image_surface_get_height(app.settings_gear_svg));
      double display_scale = gz / std::max(svg_w, svg_h);
      cairo_save(cr);
      cairo_rectangle(cr, gox, goy, gz, gz);
      cairo_clip(cr);
      cairo_translate(cr, gox, goy);
      cairo_scale(cr, display_scale, display_scale);
      cairo_mask_surface(cr, app.settings_gear_svg, 0, 0);
      cairo_restore(cr);
    } else {
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 16.0 * zf);
      cairo_move_to(cr, gear_x + (gear_w - 14.0 * zf) / 2,
                     top_h / 2 + static_cast<int>(5.0 * zf));
      cairo_show_text(cr, "\u2699");
    }
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

} // namespace eh::file_browser
