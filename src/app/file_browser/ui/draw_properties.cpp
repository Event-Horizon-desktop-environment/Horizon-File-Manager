// draw_properties.cpp — Exported from ui/draw.cpp as part of the Step 4 file split.

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



void draw_properties_dialog(AppState& app, cairo_t* cr) {
  auto& p = app.properties;
  if (!p.open) return;

  const int card_w = 520;
  const int card_h = app.height >= 400 ? app.height : 560;
  const int cx = (app.width - card_w) / 2;
  const int cy = (app.height - card_h) / 2;
  const int pad = 24;

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

  cairo_text_extents_t te;

  // ── Header: left-aligned icon tile + name + meta ──
  const int hdr_x = cx + pad;
  const int hdr_y = cy + 16;
  const int tile = 52;
  hui::design::card_fill(cr, app, 0.55);
  draw_rounded_rect(cr, hdr_x, hdr_y, tile, tile, 13);
  cairo_fill(cr);
  auto icon = app.icons.tray_icon(p.multi ? "folder-multiple"
           : (p.icon_name.empty() ? (p.is_dir ? "folder" : "text-x-generic") : p.icon_name));
  if (!icon && p.multi) icon = app.icons.tray_icon("text-x-generic");
  if (icon && icon->surface) {
    double iw = icon->width, ih = icon->height;
    if (iw > 0 && ih > 0) {
      double inner = 32.0;
      double scale = inner / std::max(1.0, std::max(iw, ih));
      cairo_save(cr);
      cairo_translate(cr, hdr_x + (tile - iw * scale) / 2, hdr_y + (tile - ih * scale) / 2);
      cairo_scale(cr, scale, scale);
      cairo_set_source_surface(cr, icon->surface, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    }
  }
  double name_x = hdr_x + tile + 14;
  double name_max_w = cx + card_w - pad - 34 - name_x;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  std::string disp_name = hui::design::clip_end(cr, p.name, name_max_w);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, name_x, hdr_y + 21);
  cairo_show_text(cr, disp_name.c_str());

  // Meta line: size · friendly type (files) / item counts (dirs, multi).
  std::string meta;
  if (p.multi) {
    std::string items = std::to_string(p.file_count + p.dir_count) +
                        ((p.file_count + p.dir_count == 1) ? " item" : " items");
    meta = items + "  ·  " + (p.dir_size_pending ? "Calculating…" : hui::design::size_human(p.size));
  } else if (p.is_dir) {
    if (p.dir_size_pending) meta = "Calculating…";
    else if (p.contained_files == 0 && p.contained_dirs == 0) meta = "Empty folder";
    else {
      if (p.contained_files > 0)
        meta += std::to_string(p.contained_files) + (p.contained_files == 1 ? " file" : " files");
      if (p.contained_dirs > 0) {
        if (!meta.empty()) meta += ", ";
        meta += std::to_string(p.contained_dirs) + (p.contained_dirs == 1 ? " folder" : " folders");
      }
    }
  } else {
    meta = hui::design::size_human(p.size) + "  ·  " + hui::design::friendly_type(p.mime_type, false);
  }
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.9);
  cairo_move_to(cr, name_x, hdr_y + 41);
  cairo_show_text(cr, hui::design::clip_end(cr, meta, name_max_w).c_str());

  // Close X (top right)
  double close_x = cx + card_w - pad - 28;
  double close_y = cy + 14;
  bool close_hov = (app.pointerX >= close_x && app.pointerX < close_x + 28 &&
                    app.pointerY >= close_y && app.pointerY < close_y + 28);
  p.hit_close[0] = close_x; p.hit_close[1] = close_y;
  p.hit_close[2] = 28; p.hit_close[3] = 28;
  if (close_hov) {
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.14);
    draw_rounded_rect(cr, close_x, close_y, 28, 28, 14);
    cairo_fill(cr);
  }
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, close_hov ? 0.85 : 0.45);
  cairo_set_line_width(cr, 1.6);
  cairo_move_to(cr, close_x + 9, close_y + 9);
  cairo_line_to(cr, close_x + 19, close_y + 19);
  cairo_stroke(cr);
  cairo_move_to(cr, close_x + 19, close_y + 9);
  cairo_line_to(cr, close_x + 9, close_y + 19);
  cairo_stroke(cr);

  // ── Segmented tabs ──
  int tab_y = hdr_y + tile + 12;
  const int tab_h = 34;
  int content_of_tab[4];
  int num_tabs = 0;
  content_of_tab[num_tabs++] = 0;
  content_of_tab[num_tabs++] = 1;
  bool has_image = (p.image_w > 0 && p.image_h > 0);
  bool has_media = p.is_media;
  if (has_image) content_of_tab[num_tabs++] = 2;
  if (has_media)  content_of_tab[num_tabs++] = 3;
  const char* tab_labels[4] = {"General", "Permissions", "Image", "Media"};
  const int seg_x = cx + pad;
  const int seg_w = card_w - 2 * pad;
  hui::design::card_fill(cr, app, 0.45);
  draw_rounded_rect(cr, seg_x, tab_y, seg_w, tab_h, 10);
  cairo_fill(cr);
  const int seg_gap = 3;
  int seg_w_each = (seg_w - seg_gap * 2 - seg_gap * (num_tabs - 1)) / num_tabs;
  int seg_total = seg_w_each * num_tabs + seg_gap * (num_tabs - 1);
  int seg_x0 = seg_x + (seg_w - seg_total) / 2;
  for (int t = 0; t < num_tabs; ++t) {
    int ct = content_of_tab[t];
    int tx = seg_x0 + t * (seg_w_each + seg_gap);
    bool active = (t == p.tab);
    bool tab_hov = (app.pointerX >= tx && app.pointerX < tx + seg_w_each &&
                    app.pointerY >= tab_y && app.pointerY < tab_y + tab_h);
    p.hit_tabs[t][0] = tx; p.hit_tabs[t][1] = tab_y;
    p.hit_tabs[t][2] = seg_w_each; p.hit_tabs[t][3] = tab_h;
    if (active) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.10);
      draw_rounded_rect(cr, tx, tab_y + seg_gap, seg_w_each, tab_h - seg_gap * 2, 7);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.9);
      draw_rounded_rect(cr, tx + 14, tab_y + tab_h - 6, seg_w_each - 28, 2, 1);
      cairo_fill(cr);
    } else if (tab_hov) {
      cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.08);
      draw_rounded_rect(cr, tx, tab_y + seg_gap, seg_w_each, tab_h - seg_gap * 2, 7);
      cairo_fill(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, active ? 0.95 : 0.5);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           active ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.5);
    cairo_text_extents_t ste;
    cairo_text_extents(cr, tab_labels[ct], &ste);
    cairo_move_to(cr, tx + (seg_w_each - ste.x_advance) / 2, tab_y + tab_h / 2 + ste.height * 0.35);
    cairo_show_text(cr, tab_labels[ct]);
  }

  int content_tab = (p.tab >= 0 && p.tab < num_tabs) ? content_of_tab[p.tab] : 0;

  // ── Content area ──
  int content_y0 = tab_y + tab_h + 10;
  int content_h_max = card_h - (content_y0 - cy) - 52;
  // Clamp stale scroll offsets (e.g. after a tab switch shrank the content).
  if (p.scroll_px > std::max(0, p.content_h - content_h_max))
    p.scroll_px = std::max(0, p.content_h - content_h_max);
  if (p.scroll_px < 0) p.scroll_px = 0;
  cairo_save(cr);
  cairo_rectangle(cr, cx + pad - 14, content_y0, card_w - 2 * pad + 28, content_h_max);
  cairo_clip(cr);

  int row_w = card_w - 2 * pad;
  int col1_x = cx + pad;
  int col2_x = cx + card_w - pad;
  int card_x = col1_x - 14;
  int card_w_full = row_w + 28;
  int ly = content_y0 + 4 - p.scroll_px;

  // Section title: small caps secondary.
  auto draw_section_title = [&](const char* title) {
    ly += 6;
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.75);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11);
    std::string up = title;
    for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    cairo_move_to(cr, col1_x, ly + 11);
    cairo_show_text(cr, up.c_str());
    ly += 20;
  };

  // Static key/value card. Rows are 32px with inset hairline dividers.
  // Values truncate (middle ellipsis for paths when is_path).
  auto draw_kv_card = [&](const std::vector<std::tuple<std::string, std::string, bool>>& rows) {
    if (rows.empty()) return;
    const int row_h = 32;
    int ch = static_cast<int>(rows.size()) * row_h + 8;
    int cy0 = ly + 2;
    hui::design::card_fill(cr, app, 0.55);
    draw_rounded_rect(cr, card_x, cy0, card_w_full, ch, 12);
    cairo_fill(cr);
    for (size_t i = 0; i < rows.size(); ++i) {
      int ry = cy0 + 4 + static_cast<int>(i) * row_h;
      if (i > 0) {
        cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.16);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, card_x + 14, ry + 0.5);
        cairo_line_to(cr, card_x + card_w_full - 14, ry + 0.5);
        cairo_stroke(cr);
      }
      const auto& [label, value, is_path] = rows[i];
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12);
      cairo_move_to(cr, card_x + 14, ry + 20);
      cairo_show_text(cr, label.c_str());
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12.5);
      double max_vw = card_w_full - 28 - 150;
      std::string shown = is_path ? hui::design::clip_middle(cr, value, max_vw)
                                  : hui::design::clip_end(cr, value, max_vw);
      cairo_text_extents_t ve;
      cairo_text_extents(cr, shown.c_str(), &ve);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_move_to(cr, card_x + card_w_full - 14 - ve.x_advance, ry + 20);
      cairo_show_text(cr, shown.c_str());
    }
    ly += ch + 6;
  };

  // Pending permission-dropdown overlay (drawn after unclipping so it is
  // never cut off at the content edge).
  struct PendingDD { bool armed = false; int x = 0, y = 0, w = 0, h = 0, pi = 0; };
  PendingDD pending_dd;

  // ── General tab ──
  if (content_tab == 0) {
    if (p.multi) {
      draw_section_title("Selection");
      std::string items;
      if (p.dir_count > 0 && p.file_count > 0)
        items = std::to_string(p.dir_count) + (p.dir_count == 1 ? " folder, " : " folders, ") +
                std::to_string(p.file_count) + (p.file_count == 1 ? " file" : " files");
      else if (p.dir_count > 0)
        items = std::to_string(p.dir_count) + (p.dir_count == 1 ? " folder" : " folders");
      else
        items = std::to_string(p.file_count) + (p.file_count == 1 ? " file" : " files");
      draw_kv_card({{items, "", false},
                    {"Total size", p.dir_size_pending ? "Calculating…" : hui::design::size_full(p.size),
                     false},
                    {"Location", p.location, true}});
      draw_section_title("Ownership");
      draw_kv_card({{"Owner", p.owner_name, false}, {"Group", p.group_name, false}});
    } else {
      draw_section_title("Details");
      {
        std::vector<std::tuple<std::string, std::string, bool>> rows;
        rows.emplace_back("Type", hui::design::friendly_type(p.mime_type, p.is_dir), false);
        if (p.is_dir) {
          std::string items;
          if (p.dir_size_pending) items = "Calculating…";
          else if (p.contained_files == 0 && p.contained_dirs == 0) items = "Empty";
          else if (p.contained_files > 0 && p.contained_dirs > 0)
            items = std::to_string(p.contained_files) + (p.contained_files == 1 ? " file, " : " files, ") +
                    std::to_string(p.contained_dirs) + (p.contained_dirs == 1 ? " folder" : " folders");
          else if (p.contained_files > 0)
            items = std::to_string(p.contained_files) + (p.contained_files == 1 ? " file" : " files");
          else
            items = std::to_string(p.contained_dirs) + (p.contained_dirs == 1 ? " folder" : " folders");
          rows.emplace_back("Contents", items, false);
        }
        rows.emplace_back("Size", p.dir_size_pending ? "Calculating…" : hui::design::size_full(p.size),
                            false);
        if (!p.location.empty()) rows.emplace_back("Location", p.location, true);
        draw_kv_card(rows);
      }
      draw_section_title("Dates");
      {
        std::vector<std::tuple<std::string, std::string, bool>> rows;
        std::string mod = hui::design::date_md(p.modified_sec);
        std::string cre = hui::design::date_md(p.created_sec);
        if (!mod.empty()) rows.emplace_back("Modified", mod, false);
        if (!cre.empty()) rows.emplace_back("Created", cre, false);
        draw_kv_card(rows);
      }

      // ── Tags (interactive) ──
      draw_section_title("Tags");
      {
        bool tags_hover =
            !p.multi &&
            app.pointerX >= p.hit_tags_row[0] &&
            app.pointerX < p.hit_tags_row[0] + p.hit_tags_row[2] &&
            app.pointerY >= p.hit_tags_row[1] &&
            app.pointerY < p.hit_tags_row[1] + p.hit_tags_row[3];
        const int th = p.tags_edit ? 40 : 34;
        int ty0 = ly + 2;
        hui::design::card_fill(cr, app, p.tags_edit ? 0.8 : 0.55);
        draw_rounded_rect(cr, card_x, ty0, card_w_full, th + 8, 12);
        cairo_fill(cr);
        if ((tags_hover || p.tags_edit) && !p.multi) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                                p.tags_edit ? 0.5 : 0.3);
          cairo_set_line_width(cr, 1.2);
          draw_rounded_rect(cr, card_x, ty0, card_w_full, th + 8, 12);
          cairo_stroke(cr);
        }
        p.hit_tags_row[0] = card_x; p.hit_tags_row[1] = ty0;
        p.hit_tags_row[2] = card_w_full; p.hit_tags_row[3] = th + 8;

        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12);
        cairo_move_to(cr, card_x + 14, ty0 + 24);
        cairo_show_text(cr, p.tags_edit ? "Edit tags" : "Tags");

        std::string disp = p.tags_edit ? p.tags_buf : (p.tags_value.empty() ? "Add tags…" : p.tags_value);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.5);
        if (!p.tags_edit) {
          bool empty = p.tags_value.empty();
          double max_vw = card_w_full - 28 - 130;
          std::string shown = hui::design::clip_end(cr, disp, max_vw);
          cairo_text_extents_t ve;
          cairo_text_extents(cr, shown.c_str(), &ve);
          cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, empty ? 0.45 : 1.0);
          cairo_move_to(cr, card_x + card_w_full - 14 - ve.x_advance, ty0 + 24);
          cairo_show_text(cr, shown.c_str());
        } else {
          cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
          cairo_move_to(cr, card_x + 14, ty0 + 24 + 18);
          cairo_show_text(cr, disp.c_str());
          cairo_text_extents(cr, disp.c_str(), &te);
          cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.8);
          cairo_set_line_width(cr, 1.2);
          cairo_move_to(cr, card_x + 16 + te.x_advance, ty0 + 28);
          cairo_line_to(cr, card_x + 16 + te.x_advance, ty0 + 42);
          cairo_stroke(cr);
        }
        ly += th + 8 + 6;
      }

      draw_section_title("Ownership");
      draw_kv_card({{"Owner", p.owner_name, false}, {"Group", p.group_name, false}});

      // ── Storage bar (replaces the tiny donut) ──
      if (!p.multi && p.vol_total_bytes > 0) {
        draw_section_title("Storage");
        const uint64_t used = p.vol_total_bytes - std::min(p.vol_free_bytes, p.vol_total_bytes);
        double frac = std::clamp(static_cast<double>(used) / static_cast<double>(p.vol_total_bytes), 0.0, 1.0);
        int sh = 64;
        int sy0 = ly + 2;
        hui::design::card_fill(cr, app, 0.55);
        draw_rounded_rect(cr, card_x, sy0, card_w_full, sh, 12);
        cairo_fill(cr);
        char pct[16];
        snprintf(pct, sizeof(pct), "%.0f%% used", frac * 100.0);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12.5);
        cairo_move_to(cr, card_x + 14, sy0 + 22);
        cairo_show_text(cr, pct);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.9);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.5);
        std::string free_line =
            hui::design::size_human(p.vol_free_bytes) + " free of " + hui::design::size_human(p.vol_total_bytes);
        cairo_text_extents_t ve;
        cairo_text_extents(cr, free_line.c_str(), &ve);
        cairo_move_to(cr, card_x + card_w_full - 14 - ve.x_advance, sy0 + 22);
        cairo_show_text(cr, free_line.c_str());
        // Track.
        hui::design::bar(cr, app, card_x + 14, sy0 + 34, card_w_full - 28, frac);
        ly += sh + 6;
      }
    }

    if (p.can_be_executable) {
      draw_section_title("Run");
      int ey0 = ly + 2;
      const int eh = 52;
      hui::design::card_fill(cr, app, 0.55);
      draw_rounded_rect(cr, card_x, ey0, card_w_full, eh, 12);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12.5);
      cairo_move_to(cr, card_x + 14, ey0 + 22);
      cairo_show_text(cr, "Allow running as a program");
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.85);
      cairo_set_font_size(cr, 11);
      cairo_move_to(cr, card_x + 14, ey0 + 38);
      cairo_show_text(cr, p.executable ? "Enabled — double-click will launch it" : "Disabled — opens as a file");
      int toggle_x = card_x + card_w_full - 14 - 40;
      int toggle_y = ey0 + (eh - 22) / 2;
      p.hit_exec_toggle[0] = toggle_x - 4; p.hit_exec_toggle[1] = toggle_y - 4;
      p.hit_exec_toggle[2] = 48; p.hit_exec_toggle[3] = 30;
      hui::design::draw_switch(cr, app, toggle_x, toggle_y, p.executable);
      ly += eh + 6;
    }

  // ── Permissions tab ──
  } else if (content_tab == 1) {
    draw_section_title("Access");
    {
      const char* perm_names[] = {"Owner", "Group", "Others"};
      const char* perm_subs[] = {"Full control over this file", "People in this group", "Everyone else"};
      int combo_vals[3] = {p.perm_owner, p.perm_group, p.perm_other};
      const char* combo_items[] = {"No access", "View only", "View & edit", "Full control"};
      const int combo_w = 150, combo_h = 30, row_h = 56;
      int ph = 3 * row_h + 8;
      int py0 = ly + 2;
      hui::design::card_fill(cr, app, 0.55);
      draw_rounded_rect(cr, card_x, py0, card_w_full, ph, 12);
      cairo_fill(cr);
      for (int pi = 0; pi < 3; ++pi) {
        int ry = py0 + 4 + pi * row_h;
        if (pi > 0) {
          cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.16);
          cairo_set_line_width(cr, 1);
          cairo_move_to(cr, card_x + 14, ry + 0.5);
          cairo_line_to(cr, card_x + card_w_full - 14, ry + 0.5);
          cairo_stroke(cr);
        }
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.5);
        cairo_move_to(cr, card_x + 14, ry + 22);
        cairo_show_text(cr, perm_names[pi]);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.85);
        cairo_set_font_size(cr, 11);
        cairo_move_to(cr, card_x + 14, ry + 38);
        cairo_show_text(cr, perm_subs[pi]);

        int combo_x = card_x + card_w_full - 14 - combo_w;
        int combo_y = ry + (row_h - combo_h) / 2;
        p.hit_combo[pi][0] = combo_x; p.hit_combo[pi][1] = combo_y;
        p.hit_combo[pi][2] = combo_w; p.hit_combo[pi][3] = combo_h;
        bool combo_hov = (app.pointerX >= combo_x && app.pointerX < combo_x + combo_w &&
                          app.pointerY >= combo_y && app.pointerY < combo_y + combo_h);
        bool combo_sel = (pi == p.combo_open);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, combo_hov || combo_sel ? 0.12 : 0.06);
        draw_rounded_rect(cr, combo_x, combo_y, combo_w, combo_h, 8);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                              combo_hov || combo_sel ? 0.5 : 0.22);
        cairo_set_line_width(cr, 1);
        draw_rounded_rect(cr, combo_x, combo_y, combo_w, combo_h, 8);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12);
        std::string cur = hui::design::clip_end(cr, combo_items[combo_vals[pi]], combo_w - 36);
        cairo_move_to(cr, combo_x + 10, combo_y + 19);
        cairo_show_text(cr, cur.c_str());
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.7);
        cairo_set_line_width(cr, 1.5);
        int ax = combo_x + combo_w - 15, ay = combo_y + combo_h / 2;
        cairo_move_to(cr, ax - 3, ay - 3);
        cairo_line_to(cr, ax, ay + 1);
        cairo_line_to(cr, ax + 3, ay - 3);
        cairo_stroke(cr);

        if (combo_sel) {
          int dd_item_h = 30;
          int dd_y = combo_y + combo_h + 4;
          pending_dd = {true, combo_x, dd_y, combo_w, 4 * dd_item_h, pi};
        }
      }
      ly += ph + 6;
    }

    if (!p.multi) {
      draw_section_title("Advanced");
      auto rwx_string = [](mode_t m) {
        char s[10];
        const char* rwx[] = {"---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx"};
        snprintf(s, sizeof(s), "%s%s%s", rwx[(m >> 6) & 7], rwx[(m >> 3) & 7], rwx[m & 7]);
        return std::string(s);
      };
      bool oct_hover =
          app.pointerX >= p.hit_octal[0] && app.pointerX < p.hit_octal[0] + p.hit_octal[2] &&
          app.pointerY >= p.hit_octal[1] && app.pointerY < p.hit_octal[1] + p.hit_octal[3];
      int oy0 = ly + 2;
      const int oh = p.octal_edit ? 62 : 52;
      hui::design::card_fill(cr, app, p.octal_edit ? 0.8 : 0.55);
      draw_rounded_rect(cr, card_x, oy0, card_w_full, oh, 12);
      cairo_fill(cr);
      p.hit_octal[0] = card_x; p.hit_octal[1] = oy0;
      p.hit_octal[2] = card_w_full; p.hit_octal[3] = oh;
      if (oct_hover || p.octal_edit) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, p.octal_edit ? 0.5 : 0.3);
        cairo_set_line_width(cr, 1.2);
        draw_rounded_rect(cr, card_x, oy0, card_w_full, oh, 12);
        cairo_stroke(cr);
      }
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12.5);
      cairo_move_to(cr, card_x + 14, oy0 + 22);
      cairo_show_text(cr, "Numeric mode");
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.85);
      cairo_set_font_size(cr, 11);
      cairo_move_to(cr, card_x + 14, oy0 + 38);
      cairo_show_text(cr, p.octal_edit ? "Type 3–4 octal digits" : "Click to edit with chmod digits");
      if (!p.octal_edit) {
        char ob[16];
        snprintf(ob, sizeof(ob), "%lo", static_cast<unsigned long>(p.current_mode & 07777));
        std::string val = rwx_string(p.current_mode & 07777) + "  ·  " + ob;
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12.5);
        cairo_text_extents_t ve;
        cairo_text_extents(cr, val.c_str(), &ve);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_move_to(cr, card_x + card_w_full - 14 - ve.x_advance, oy0 + 30);
        cairo_show_text(cr, val.c_str());
      } else {
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 14);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_move_to(cr, card_x + card_w_full - 14 - 90, oy0 + 26);
        cairo_show_text(cr, p.octal_buf.c_str());
        cairo_text_extents(cr, p.octal_buf.c_str(), &te);
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.9);
        cairo_set_line_width(cr, 1.4);
        cairo_move_to(cr, card_x + card_w_full - 12 - 90 + te.x_advance, oy0 + 14);
        cairo_line_to(cr, card_x + card_w_full - 12 - 90 + te.x_advance, oy0 + 30);
        cairo_stroke(cr);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.8);
        const char* hint = "Enter applies  ·  Esc cancels";
        cairo_move_to(cr, card_x + 14, oy0 + 52);
        cairo_show_text(cr, hint);
      }
      ly += oh + 6;
    }

    if (!p.is_dir) {
      draw_section_title("Run");
      int ey0 = ly + 2;
      const int eh = 52;
      hui::design::card_fill(cr, app, 0.55);
      draw_rounded_rect(cr, card_x, ey0, card_w_full, eh, 12);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12.5);
      cairo_move_to(cr, card_x + 14, ey0 + 22);
      cairo_show_text(cr, "Allow running as a program");
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.85);
      cairo_set_font_size(cr, 11);
      cairo_move_to(cr, card_x + 14, ey0 + 38);
      cairo_show_text(cr, p.executable ? "Enabled — double-click will launch it" : "Disabled — opens as a file");
      int toggle_x = card_x + card_w_full - 14 - 40;
      int toggle_y = ey0 + (eh - 22) / 2;
      p.hit_exec_toggle[0] = toggle_x - 4; p.hit_exec_toggle[1] = toggle_y - 4;
      p.hit_exec_toggle[2] = 48; p.hit_exec_toggle[3] = 30;
      hui::design::draw_switch(cr, app, toggle_x, toggle_y, p.executable);
      ly += eh + 6;
    }

  // ── Image tab ── hero summary + 2-col spec grid ──
  } else if (content_tab == 2) {
    long long total = static_cast<long long>(p.image_w) * static_cast<long long>(p.image_h);
    char area[64];
    const char* area_label = "Megapixels";
    if (total >= 1000000000LL) {
      area_label = "Gigapixels";
      snprintf(area, sizeof(area), "%.2f GP (%.1f MP)", total / 1e9, total / 1e6);
    } else if (total >= 1000000LL) {
      double mp = total / 1e6;
      if (total >= 100000000LL)
        snprintf(area, sizeof(area), "%.1f MP (%.2f GP)", mp, total / 1e9);
      else
        snprintf(area, sizeof(area), "%.1f MP", mp);
    } else if (total >= 1000LL) {
      area_label = "Kilopixels";
      snprintf(area, sizeof(area), "%.1f KP (%.2f MP)", total / 1e3, total / 1e6);
    } else {
      area_label = "Pixels";
      snprintf(area, sizeof(area), "%lld px", total);
    }

    double ratio = (p.image_h > 0) ? static_cast<double>(p.image_w) / p.image_h : 1.0;
    const char* orient = (p.image_w > p.image_h) ? "Landscape" : (p.image_w < p.image_h) ? "Portrait" : "Square";
    char ratio_str[32];
    if (p.image_w >= p.image_h) snprintf(ratio_str, sizeof(ratio_str), "%.2f : 1", ratio);
    else snprintf(ratio_str, sizeof(ratio_str), "1 : %.2f", 1.0 / std::max(ratio, 1e-6));

    // ── Hero: aspect box + dimensions + MP + orientation ──
    {
      const int hero_h = 90;
      const int hero_y = ly + 2;
      hui::design::card_fill(cr, app, 0.55);
      draw_rounded_rect(cr, card_x, hero_y, card_w_full, hero_h, 12);
      cairo_fill(cr);

      const int slot_x = card_x + 16, slot_w = 88;
      const int slot_h = hero_h - 28;
      const int slot_y = hero_y + 14;
      double bw = slot_w, bh = slot_w / std::max(ratio, 0.05);
      if (bh > slot_h) { bh = slot_h; bw = slot_h * std::min(ratio, 20.0); }
      bw = std::max<double>(bw, 10); bh = std::max<double>(bh, 10);
      double bx = slot_x + (slot_w - bw) / 2, by = slot_y + (slot_h - bh) / 2;
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      draw_rounded_rect(cr, bx, by, bw, bh, 4);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.55);
      cairo_set_line_width(cr, 1.2);
      draw_rounded_rect(cr, bx, by, bw, bh, 4);
      cairo_stroke(cr);

      double tx = slot_x + slot_w + 14;
      double max_tw = card_x + card_w_full - 14 - tx;
      std::string dims = hui::design::group_int(p.image_w) + " \u00d7 " + hui::design::group_int(p.image_h);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 17);
      std::string dims_show = hui::design::clip_end(cr, dims, max_tw);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_move_to(cr, tx, hero_y + 30);
      cairo_show_text(cr, dims_show.c_str());
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12.5);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
      cairo_move_to(cr, tx, hero_y + 50);
      cairo_show_text(cr, area);
      std::string sub = std::string(orient) + "  \u00b7  " + ratio_str;
      cairo_set_font_size(cr, 11.5);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.8);
      cairo_move_to(cr, tx, hero_y + 69);
      cairo_show_text(cr, sub.c_str());

      ly += hero_h + 8;
    }

    // ── Spec grid (2 columns) ──
    {
      std::vector<std::pair<std::string, std::string>> specs;
      std::string comp = p.image_compression;
      if (!comp.empty() && comp != "Undef" && comp != "Undefined") {
        if (comp == "Zip") comp = "Deflate";
        specs.emplace_back("Compression", comp);
      }
      if (!p.image_colorspace.empty()) specs.emplace_back("Color space", p.image_colorspace);
      if (!p.image_bit_depth.empty()) {
        std::string bd = p.image_bit_depth;
        bd += (bd.find(',') == std::string::npos) ? " bpc" : "-bit";
        specs.emplace_back("Bit depth", bd);
      }
      specs.emplace_back("Transparency", p.image_has_alpha ? "Yes" : "No");
      if (!p.image_resolution.empty()) {
        std::string res = p.image_resolution;
        auto cross = res.find("\u00d7");
        std::string res_show = res;
        if (cross != std::string::npos) {
          auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t");
            return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
          };
          std::string l = trim(res.substr(0, cross)), r = trim(res.substr(cross + 2));
          if (l == r) res_show = l;
        }
        if (!p.image_res_unit.empty()) res_show += " " + p.image_res_unit;
        specs.emplace_back("Resolution", res_show);
      }
      {
        std::string fmt;
        auto slash = p.mime_type.find('/');
        if (slash != std::string::npos) {
          fmt = p.mime_type.substr(slash + 1);
          auto plus = fmt.find('+');
          if (plus != std::string::npos) fmt = fmt.substr(0, plus);
          if (fmt.rfind("x-", 0) == 0) fmt = fmt.substr(2);
          for (auto& c : fmt) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (fmt.empty()) {
          auto d = p.name.rfind('.');
          if (d != std::string::npos) {
            fmt = p.name.substr(d + 1);
            for (auto& c : fmt) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          }
        }
        if (!fmt.empty()) specs.emplace_back("Format", fmt);
      }
      specs.insert(specs.begin(), {area_label, area});
      specs.insert(specs.begin() + 1, {"Aspect", ratio_str});

      const int gap = 8, cell_h = 52;
      const int cell_w = (card_w_full - gap) / 2;
      for (size_t i = 0; i < specs.size(); ++i) {
        int r = static_cast<int>(i) / 2, c = static_cast<int>(i) % 2;
        bool solo_last = (i == specs.size() - 1) && (specs.size() % 2 == 1);
        int cw = solo_last ? card_w_full : cell_w;
        int cell_x = solo_last ? card_x : card_x + c * (cell_w + gap);
        int cell_y = ly + 2 + r * (cell_h + gap);
        hui::design::card_fill(cr, app, 0.55);
        draw_rounded_rect(cr, cell_x, cell_y, cw, cell_h, 10);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.9);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10.5);
        cairo_move_to(cr, cell_x + 12, cell_y + 18);
        cairo_show_text(cr, specs[i].first.c_str());

        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12.5);
        std::string val = hui::design::clip_end(cr, specs[i].second, cw - 24);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_move_to(cr, cell_x + 12, cell_y + 37);
        cairo_show_text(cr, val.c_str());
      }
      int rows = static_cast<int>((specs.size() + 1) / 2);
      ly += rows * cell_h + (rows - 1) * gap + 8;
    }

  // ── Media tab ──
  } else if (content_tab == 3) {
    auto pretty_codec = [](std::string c) {
      if (!c.empty()) c[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(c[0])));
      return c;
    };
    if (p.media_duration > 0 || !p.container.empty()) {
      draw_section_title("File");
      std::vector<std::tuple<std::string, std::string, bool>> rows;
      if (p.media_duration > 0) {
        int total_sec = static_cast<int>(p.media_duration);
        int hrs = total_sec / 3600, mins = (total_sec % 3600) / 60, secs = total_sec % 60;
        char dur[32];
        if (hrs > 0) snprintf(dur, sizeof(dur), "%d:%02d:%02d", hrs, mins, secs);
        else snprintf(dur, sizeof(dur), "%d:%02d", mins, secs);
        rows.emplace_back("Duration", dur, false);
      }
      if (!p.container.empty()) rows.emplace_back("Container", p.container, false);
      draw_kv_card(rows);
    }
    if (p.has_video) {
      draw_section_title("Video");
      std::vector<std::tuple<std::string, std::string, bool>> rows;
      if (!p.video_codec.empty()) rows.emplace_back("Codec", pretty_codec(p.video_codec), false);
      if (p.video_w > 0 && p.video_h > 0) {
        char vdim[64];
        snprintf(vdim, sizeof(vdim), "%d \u00d7 %d px", p.video_w, p.video_h);
        rows.emplace_back("Size", vdim, false);
      }
      if (!p.video_framerate.empty()) rows.emplace_back("Frame rate", p.video_framerate + " fps", false);
      if (p.video_bitrate > 0) {
        char vbr[32];
        if (p.video_bitrate >= 1000000) snprintf(vbr, sizeof(vbr), "%.1f Mbps", p.video_bitrate / 1000000.0);
        else snprintf(vbr, sizeof(vbr), "%d kbps", p.video_bitrate / 1000);
        rows.emplace_back("Bitrate", vbr, false);
      }
      draw_kv_card(rows);
    }
    if (p.has_audio) {
      draw_section_title("Audio");
      std::vector<std::tuple<std::string, std::string, bool>> rows;
      if (!p.audio_codec.empty()) rows.emplace_back("Codec", pretty_codec(p.audio_codec), false);
      if (p.audio_sample_rate > 0) {
        char sr[32];
        if (p.audio_sample_rate >= 1000) snprintf(sr, sizeof(sr), "%.1f kHz", p.audio_sample_rate / 1000.0);
        else snprintf(sr, sizeof(sr), "%d Hz", p.audio_sample_rate);
        rows.emplace_back("Sample rate", sr, false);
      }
      if (p.audio_channels > 0) {
        static const char* ch_names[] = {"Mono", "Stereo", "2.1", "Quad", "5.0", "5.1", "6.1", "7.1"};
        std::string ch = (p.audio_channels >= 1 && p.audio_channels <= 8)
          ? ch_names[p.audio_channels - 1]
          : std::to_string(p.audio_channels) + " channels";
        rows.emplace_back("Channels", ch, false);
      }
      if (p.audio_bitrate > 0) {
        char abr[32];
        if (p.audio_bitrate >= 1000000) snprintf(abr, sizeof(abr), "%.1f Mbps", p.audio_bitrate / 1000000.0);
        else snprintf(abr, sizeof(abr), "%d kbps", p.audio_bitrate / 1000);
        rows.emplace_back("Bitrate", abr, false);
      }
      draw_kv_card(rows);
    }
  }

  p.content_h = ly - (content_y0 - p.scroll_px);
  // Window height that shows this tab with no scrolling + bottom breathing.
  p.desired_h = (content_y0 - cy) + p.content_h + 52 + 12;
  cairo_restore(cr);

  // ── Permission dropdown overlay (outside the content clip) ──
  if (pending_dd.armed) {
    const char* combo_items[] = {"No access", "View only", "View & edit", "Full control"};
    int dd_item_h = 30;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
    draw_rounded_rect(cr, pending_dd.x + 1, pending_dd.y + 2, pending_dd.w, pending_dd.h, 8);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
    draw_rounded_rect(cr, pending_dd.x, pending_dd.y, pending_dd.w, pending_dd.h, 8);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
    cairo_set_line_width(cr, 1);
    draw_rounded_rect(cr, pending_dd.x + 0.5, pending_dd.y + 0.5, pending_dd.w - 1, pending_dd.h - 1, 7.5);
    cairo_stroke(cr);
    int cur = 0;
    if (pending_dd.pi == 0) cur = p.perm_owner;
    else if (pending_dd.pi == 1) cur = p.perm_group;
    else cur = p.perm_other;
    for (int ci = 0; ci < 4; ++ci) {
      int item_y = pending_dd.y + ci * dd_item_h;
      bool item_hov = (app.pointerX >= pending_dd.x && app.pointerX < pending_dd.x + pending_dd.w &&
                       app.pointerY >= item_y && app.pointerY < item_y + dd_item_h);
      bool item_sel = (ci == cur);
      p.hit_combo_items[pending_dd.pi][ci][0] = pending_dd.x;
      p.hit_combo_items[pending_dd.pi][ci][1] = item_y;
      p.hit_combo_items[pending_dd.pi][ci][2] = pending_dd.w;
      p.hit_combo_items[pending_dd.pi][ci][3] = dd_item_h;
      if (item_hov || item_sel) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, item_hov ? 0.22 : 0.10);
        draw_rounded_rect(cr, pending_dd.x + 4, item_y + 2, pending_dd.w - 8, dd_item_h - 4, 5);
        cairo_fill(cr);
      }
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             item_sel ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12);
      cairo_move_to(cr, pending_dd.x + 12, item_y + 19);
      cairo_show_text(cr, combo_items[ci]);
    }
  } else {
    for (int pi = 0; pi < 3; ++pi)
      for (int ci = 0; ci < 4; ++ci)
        p.hit_combo_items[pi][ci][2] = 0;
  }

  // ── Footer: primary Close ──
  int btn_w = 96;
  int btn_h = 34;
  int btn_x = cx + card_w - pad - btn_w;
  int btn_y = cy + card_h - 48;
  bool btn_hov = (app.pointerX >= btn_x && app.pointerX < btn_x + btn_w &&
                  app.pointerY >= btn_y && app.pointerY < btn_y + btn_h);
  p.hit_close_btn[0] = btn_x; p.hit_close_btn[1] = btn_y;
  p.hit_close_btn[2] = btn_w; p.hit_close_btn[3] = btn_h;

  hui::design::button(cr, app, btn_x, btn_y, btn_w, btn_h, "Close", true, btn_hov);
}

} // namespace eh::file_browser
