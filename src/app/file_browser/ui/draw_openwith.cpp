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

// ── Open With dialog ──────────────────────────────────────────────

void draw_open_with(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;

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

  // Card (fully opaque, layered soft shadow)
  draw_dialog_card(app, cr, cx, cy, card_w, card_h, 16);

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

} // namespace eh::file_browser
