// draw_settings.cpp — Exported from ui/draw.cpp as part of the Step 4 file split.

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


void draw_settings_dialog(AppState& app, cairo_t* cr) {
  int card_w = settings_dialog_width();
  int card_h = settings_dialog_card_height(app);
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
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, dlg_bg_alpha);
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
  int tab_w = (card_w - 2 * pad) / 3;
  const char* tab_names[] = {"General", "Appearance", "Preview"};
  for (int t = 0; t < 3; ++t) {
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

    ly = content_y + 120;

    // Independent views per directory toggle
    // (skipped while the terminal dropdown is open so the list stays on top)
    if (!app.settings_dropdown_open) {
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Independent views per folder");

    {
      double iv_toggle_x = left_x + 220;
      double iv_toggle_y = ly - 2;
      double iv_toggle_w = 40;
      double iv_toggle_h = 22;
      app.settings_hit_indep_views_toggle[0] = iv_toggle_x;
      app.settings_hit_indep_views_toggle[1] = iv_toggle_y;
      app.settings_hit_indep_views_toggle[2] = iv_toggle_w;
      app.settings_hit_indep_views_toggle[3] = iv_toggle_h;

      bool iv_hov = (app.pointerX >= iv_toggle_x && app.pointerX < iv_toggle_x + iv_toggle_w &&
                     app.pointerY >= iv_toggle_y && app.pointerY < iv_toggle_y + iv_toggle_h);
      if (iv_hov) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.1);
        draw_rounded_rect(cr, iv_toggle_x - 2, iv_toggle_y - 2, iv_toggle_w + 4, iv_toggle_h + 4, iv_toggle_h / 2 + 2);
        cairo_fill(cr);
      }

      // Toggle track
      cairo_set_source_rgba(cr, app.settings_independent_dir_views ? app.accent_r : app.outline_r,
                            app.settings_independent_dir_views ? app.accent_g : app.outline_g,
                            app.settings_independent_dir_views ? app.accent_b : app.outline_b,
                            0.6);
      draw_rounded_rect(cr, iv_toggle_x, iv_toggle_y, iv_toggle_w, iv_toggle_h, iv_toggle_h / 2);
      cairo_fill(cr);

      // Toggle knob
      double iv_knob_x = app.settings_independent_dir_views ? iv_toggle_x + iv_toggle_w - iv_toggle_h : iv_toggle_x;
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
      cairo_arc(cr, iv_knob_x + iv_toggle_h / 2, iv_toggle_y + iv_toggle_h / 2, iv_toggle_h / 2 - 2, 0, 2 * M_PI);
      cairo_fill(cr);
    }
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

    // Color engine sync toggle (Event Horizon wallpaper palette)
    ly += 52;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Color engine sync");

    double ce_toggle_x = left_x + 220;
    double ce_toggle_y = ly - 2;
    double ce_toggle_w = 40;
    double ce_toggle_h = 22;
    app.settings_hit_color_engine_toggle[0] = ce_toggle_x;
    app.settings_hit_color_engine_toggle[1] = ce_toggle_y;
    app.settings_hit_color_engine_toggle[2] = ce_toggle_w;
    app.settings_hit_color_engine_toggle[3] = ce_toggle_h;

    bool ce_hov = (app.pointerX >= ce_toggle_x && app.pointerX < ce_toggle_x + ce_toggle_w &&
                   app.pointerY >= ce_toggle_y && app.pointerY < ce_toggle_y + ce_toggle_h);
    if (ce_hov) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.1);
      draw_rounded_rect(cr, ce_toggle_x - 2, ce_toggle_y - 2, ce_toggle_w + 4, ce_toggle_h + 4, ce_toggle_h / 2 + 2);
      cairo_fill(cr);
    }

    cairo_set_source_rgba(cr, app.settings_color_engine ? app.accent_r : app.outline_r,
                          app.settings_color_engine ? app.accent_g : app.outline_g,
                          app.settings_color_engine ? app.accent_b : app.outline_b,
                          0.6);
    draw_rounded_rect(cr, ce_toggle_x, ce_toggle_y, ce_toggle_w, ce_toggle_h, ce_toggle_h / 2);
    cairo_fill(cr);

    double ce_knob_x = app.settings_color_engine ? ce_toggle_x + ce_toggle_w - ce_toggle_h : ce_toggle_x;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, ce_knob_x + ce_toggle_h / 2, ce_toggle_y + ce_toggle_h / 2, ce_toggle_h / 2 - 2, 0, 2 * M_PI);
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

  // ── Preview tab: hover preview scale (1.0-10.0, real time) ──
  if (app.settings_tab == 2) {
    const int ly = content_y;
    const int left_x = cx + pad + 8;

    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, left_x, ly + 14);
    cairo_show_text(cr, "Preview scale");

    char sc_str[16];
    snprintf(sc_str, sizeof(sc_str), "%.1f\u00d7", app.settings_preview_scale);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
    cairo_move_to(cr, left_x + 180, ly + 14);
    cairo_show_text(cr, sc_str);

    const int slider_x = cx + pad + 8;
    const int slider_y = ly + 24;
    const int slider_w = card_w - 2 * pad - 16;
    const int slider_h = 6;

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, slider_x, slider_y, slider_w, slider_h, 3);
    cairo_fill(cr);

    const double t = std::clamp(
        (app.settings_preview_scale - 1.0) / 9.0, 0.0, 1.0);
    const double fill_w = slider_w * t;
    if (fill_w > 0) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
      draw_rounded_rect(cr, slider_x, slider_y, fill_w, slider_h, 3);
      cairo_fill(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_arc(cr, slider_x + fill_w, slider_y + slider_h / 2, 8, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 0.8);
    cairo_set_font_size(cr, 12);
    cairo_move_to(cr, left_x, ly + 66);
    cairo_show_text(cr, "Hover preview size multiplier.");
    cairo_move_to(cr, left_x, ly + 86);
    cairo_show_text(cr, "Applies in real time to image previews.");
  }
}

} // namespace eh::file_browser

