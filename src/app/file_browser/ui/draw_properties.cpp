// draw_properties.cpp — Exported from ui/draw.cpp as part of the Step 4 file split.

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
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, prp_bg_alpha);
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
      std::string items_str;
      if (p.contained_files == 0 && p.contained_dirs == 0) {
        items_str = "Empty";
      } else if (p.contained_files > 0 && p.contained_dirs > 0) {
        items_str = std::to_string(p.contained_files) +
                    (p.contained_files == 1 ? " file, " : " files, ") +
                    std::to_string(p.contained_dirs) +
                    (p.contained_dirs == 1 ? " folder" : " folders");
      } else if (p.contained_files > 0) {
        items_str = std::to_string(p.contained_files) +
                    (p.contained_files == 1 ? " file" : " files");
      } else {
        items_str = std::to_string(p.contained_dirs) +
                    (p.contained_dirs == 1 ? " folder" : " folders");
      }
      draw_info_row("Contents", items_str);
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

    // ── Tags row (freedesktop user.xdg.tags) — click to edit ──
    {
      bool tags_hover =
          !p.multi &&
          app.pointerX >= p.hit_tags_row[0] &&
          app.pointerX < p.hit_tags_row[0] + p.hit_tags_row[2] &&
          app.pointerY >= p.hit_tags_row[1] &&
          app.pointerY < p.hit_tags_row[1] + p.hit_tags_row[3];
      double tag_pill_r = (app.surface_r + app.bg_r) * 0.5 + 0.12;
      double tag_pill_g = (app.surface_g + app.bg_g) * 0.5 + 0.12;
      double tag_pill_b = (app.surface_b + app.bg_b) * 0.5 + 0.12;
      cairo_set_source_rgba(cr, tag_pill_r, tag_pill_g, tag_pill_b,
                            p.tags_edit ? 0.95 : 0.75);
      draw_rounded_rect(cr, col1_x - 14, ly + 2, row_w + 28, 28, 14);
      cairo_fill(cr);
      if ((tags_hover || p.tags_edit) && !p.multi) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                              p.tags_edit ? 0.45 : 0.25);
        cairo_set_line_width(cr, 1.2);
        draw_rounded_rect(cr, col1_x - 14, ly + 2, row_w + 28, 28, 14);
        cairo_stroke(cr);
        p.hit_tags_row[0] = col1_x - 14;
        p.hit_tags_row[1] = ly + 2;
        p.hit_tags_row[2] = row_w + 28;
        p.hit_tags_row[3] = 28;
      } else if (!p.multi) {
        p.hit_tags_row[0] = col1_x - 14;
        p.hit_tags_row[1] = ly + 2;
        p.hit_tags_row[2] = row_w + 28;
        p.hit_tags_row[3] = 28;
      }

      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12);
      cairo_move_to(cr, col1_x + 2, ly + 18);
      cairo_show_text(cr, "Tags");

      std::string tags_disp =
          p.tags_edit ? p.tags_buf
                      : (p.tags_value.empty() ? "\u2014" : p.tags_value);
      double tags_max_w = row_w - 64;
      cairo_text_extents(cr, tags_disp.c_str(), &te);
      if (!p.tags_edit) {
        while (!tags_disp.empty() && te.x_advance > tags_max_w) {
          tags_disp.pop_back();
          cairo_text_extents(cr, (tags_disp + "\u2026").c_str(), &te);
        }
        if (tags_disp.empty()) tags_disp = "\u2026";
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_text_extents(cr, tags_disp.c_str(), &te);
        double tx = col2_x - te.x_advance + 2;
        if (tx < col1_x + 70) tx = col1_x + 70;
        cairo_move_to(cr, tx, ly + 18);
        cairo_show_text(cr, tags_disp.c_str());
      } else {
        // Edit mode: left-aligned text with caret, accent underline
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_move_to(cr, col1_x + 70, ly + 18);
        cairo_show_text(cr, tags_disp.c_str());
        cairo_text_extents(cr, tags_disp.c_str(), &te);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.8);
        cairo_set_line_width(cr, 1.2);
        cairo_move_to(cr, col1_x + 72 + te.x_advance, ly + 7);
        cairo_line_to(cr, col1_x + 72 + te.x_advance, ly + 21);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, col1_x + 68, ly + 26);
        cairo_line_to(cr, col2_x, ly + 26);
        cairo_stroke(cr);
      }
      ly += 32;
    }

    draw_section("Ownership");
    draw_info_row("Owner", p.owner_name);
    draw_info_row("Group", p.group_name);

    // ── Volume usage donut (filesystem holding this item) ──
    if (!p.multi && p.vol_total_bytes > 0) {
      draw_section("Volume");
      auto fmt_vol = [](uint64_t bytes) {
        char buf[48];
        double v = static_cast<double>(bytes);
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int ui = 0;
        while (v >= 1024.0 && ui < 4) { v /= 1024.0; ++ui; }
        snprintf(buf, sizeof(buf), "%.1f %s", v, units[ui]);
        return std::string(buf);
      };
      const uint64_t used_bytes =
          p.vol_total_bytes - std::min(p.vol_free_bytes, p.vol_total_bytes);
      double frac = static_cast<double>(used_bytes) /
                    static_cast<double>(p.vol_total_bytes);
      frac = std::clamp(frac, 0.0, 1.0);

      const double dcx = col1_x + 20;
      const double dcy = ly + 24;
      const double rad = 15.0;
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
      cairo_set_line_width(cr, 6);
      cairo_arc(cr, dcx, dcy, rad, 0, 2 * M_PI);
      cairo_stroke(cr);
      if (frac > 0.001) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.9);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_arc(cr, dcx, dcy, rad, -M_PI / 2, -M_PI / 2 + frac * 2 * M_PI);
        cairo_stroke(cr);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
      }
      char pct_buf[16];
      snprintf(pct_buf, sizeof(pct_buf), "%.0f%%", frac * 100.0);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 9);
      cairo_text_extents(cr, pct_buf, &te);
      cairo_move_to(cr, dcx - te.x_advance / 2, dcy + te.height / 2);
      cairo_show_text(cr, pct_buf);

      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      std::string used_line = "Used " + fmt_vol(used_bytes) +
                              " of " + fmt_vol(p.vol_total_bytes);
      cairo_move_to(cr, col1_x + 48, ly + 18);
      cairo_show_text(cr, used_line.c_str());
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 0.85);
      cairo_set_font_size(cr, 11);
      std::string free_line = fmt_vol(p.vol_free_bytes) + " free";
      cairo_move_to(cr, col1_x + 48, ly + 36);
      cairo_show_text(cr, free_line.c_str());
      ly += 52;
    }
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

    // ── Numeric (octal) mode editor — click to type, Enter applies chmod ──
    if (!p.multi) {
      draw_section("Numeric mode");
      auto rwx_string = [](mode_t m) {
        char s[10];
        const char* rwx[] = {"---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx"};
        snprintf(s, sizeof(s), "%s%s%s", rwx[(m >> 6) & 7], rwx[(m >> 3) & 7],
                 rwx[m & 7]);
        return std::string(s);
      };
      bool oct_hover =
          app.pointerX >= p.hit_octal[0] && app.pointerX < p.hit_octal[0] + p.hit_octal[2] &&
          app.pointerY >= p.hit_octal[1] && app.pointerY < p.hit_octal[1] + p.hit_octal[3];
      double oct_pill_r = (app.surface_r + app.bg_r) * 0.5 + 0.12;
      double oct_pill_g = (app.surface_g + app.bg_g) * 0.5 + 0.12;
      double oct_pill_b = (app.surface_b + app.bg_b) * 0.5 + 0.12;
      cairo_set_source_rgba(cr, oct_pill_r, oct_pill_g, oct_pill_b,
                            p.octal_edit ? 0.95 : 0.75);
      draw_rounded_rect(cr, col1_x - 14, ly + 2, row_w + 28, 28, 14);
      cairo_fill(cr);
      p.hit_octal[0] = col1_x - 14;
      p.hit_octal[1] = ly + 2;
      p.hit_octal[2] = row_w + 28;
      p.hit_octal[3] = 28;
      if (oct_hover || p.octal_edit) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                              p.octal_edit ? 0.45 : 0.25);
        cairo_set_line_width(cr, 1.2);
        draw_rounded_rect(cr, col1_x - 14, ly + 2, row_w + 28, 28, 14);
        cairo_stroke(cr);
      }

      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12);
      cairo_move_to(cr, col1_x + 2, ly + 18);
      cairo_show_text(cr, "Octal");

      if (!p.octal_edit) {
        std::string val = rwx_string(p.current_mode & 07777);
        char ob[16];
        snprintf(ob, sizeof(ob), "%lo", static_cast<unsigned long>(p.current_mode & 07777));
        val += "   (" + std::string(ob) + ")";
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_text_extents(cr, val.c_str(), &te);
        cairo_move_to(cr, col2_x - te.x_advance + 2, ly + 18);
        cairo_show_text(cr, val.c_str());
      } else {
        // Edit mode: typed digits with caret + hint
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13);
        cairo_move_to(cr, col1_x + 70, ly + 19);
        cairo_show_text(cr, p.octal_buf.c_str());
        cairo_text_extents(cr, p.octal_buf.c_str(), &te);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.8);
        cairo_set_line_width(cr, 1.2);
        cairo_move_to(cr, col1_x + 72 + te.x_advance, ly + 7);
        cairo_line_to(cr, col1_x + 72 + te.x_advance, ly + 21);
        cairo_stroke(cr);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, 0.7);
        const char* hint = "Enter applies \u00b7 Esc cancels";
        cairo_text_extents(cr, hint, &te);
        cairo_move_to(cr, col2_x - te.x_advance + 2, ly + 18);
        cairo_show_text(cr, hint);
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

} // namespace eh::file_browser

