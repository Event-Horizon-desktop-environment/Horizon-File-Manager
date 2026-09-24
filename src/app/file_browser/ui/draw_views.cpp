// draw_views.cpp — Exported from ui/draw.cpp as part of the Step 4 file split.

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


// ── list view ────────────────────────────────────────────────────

// Shared Group By header band (list/grid/compact). pinned=true adds a
// shadow so the sticky header reads as floating above content.
static void draw_group_header_band(AppState& app, cairo_t* cr, int x, int y,
                                    int w, int h, const std::string& label,
                                    bool pinned = false) {
  double zf = app.zoom_pct / 100.0;
  if (pinned) {
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.18);
    cairo_rectangle(cr, x, y + h, w, 3);
    cairo_fill(cr);
  }
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b,
                        pinned ? 1.0 : 1.0);
  cairo_rectangle(cr, x, y, w, h);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.06);
  cairo_rectangle(cr, x, y, w, h);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.5);
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 11.0 * zf);
  cairo_move_to(cr, x + static_cast<int>(44.0 * zf), y + h - 6);
  cairo_show_text(cr, label.c_str());
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.12);
  cairo_move_to(cr, x, y + h - 1);
  cairo_line_to(cr, x + w, y + h - 1);
  cairo_stroke(cr);
}

static void draw_column_header(AppState& app, cairo_t* cr, int x, int y, int w, int h,
                                const char* label,
                                bool divider_hover) {
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
  cairo_move_to(cr, x + 6, y + h / 2 + 4);
  cairo_show_text(cr, label);

  // Divider line
  if (divider_hover) {
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.6);
  } else {
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  }
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, x + w, y);
  cairo_line_to(cr, x + w, y + h);
  cairo_stroke(cr);
}

// ── draw_tab_bar ─────────────────────────────────────────────────

void draw_tab_bar(AppState& app, cairo_t* cr, int w, int tab_h, int y0, int pane_x, int pane_w) {
  double zf = app.zoom_pct / 100.0;

  int tab_count = static_cast<int>(app.tabs.size());
  app.tab_hits.resize(tab_count);

  int sidebar_w;
  if (pane_w > 0) {
    sidebar_w = pane_x;
  } else {
    sidebar_w = app.sidebar_w();
  }

  int x = sidebar_w;
  int close_icon_sz = static_cast<int>(7.0 * zf);
  int pad = static_cast<int>(12.0 * zf);
  int font_size = static_cast<int>(14.0 * zf);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, font_size);

  bool dragging = app.tab_dragging;
  int drag_sb_idx = dragging ? app.tab_drag_from : -1;

  // ── Pass 1: measure every tab so overflow can be distributed fairly ──
  int close_w = close_icon_sz + pad;
  int min_tab_w = static_cast<int>(100.0 * zf);
  std::vector<std::string> tab_labels(tab_count);
  std::vector<int> tab_final_w(tab_count, min_tab_w);
  {
    std::vector<int> want_w(tab_count, min_tab_w);
    long total_w = 0;
    for (int i = 0; i < tab_count; ++i) {
      std::string label = app.tabs[i].current_path;
      auto pos = label.rfind('/');
      if (pos != std::string::npos) label = label.substr(pos + 1);
      if (label.empty()) label = "/";
      tab_labels[i] = label;
      cairo_text_extents_t te;
      cairo_text_extents(cr, label.c_str(), &te);
      want_w[i] = std::max(pad + static_cast<int>(te.x_advance) + pad +
                               close_w + pad,
                           min_tab_w);
      total_w += want_w[i];
    }
    int avail_total = w - x;
    if (total_w <= avail_total || tab_count == 0) {
      for (int i = 0; i < tab_count; ++i)
        tab_final_w[i] = want_w[i];
    } else {
      // Proportional shrink with a floor so every tab stays clickable.
      double scale =
          static_cast<double>(avail_total) / static_cast<double>(total_w);
      int floor_w = std::max(static_cast<int>(56.0 * zf),
                             std::min(min_tab_w,
                                      avail_total / std::max(1, tab_count)));
      for (int i = 0; i < tab_count; ++i)
        tab_final_w[i] =
            std::max(floor_w, static_cast<int>(want_w[i] * scale));
    }
  }

  for (int i = 0; i < tab_count; ++i) {
    int tab_w = tab_final_w[i];

    // Elide the label into its allotted share of the bar
    {
      int budget = tab_w - pad * 3 - close_w;
      if (budget > 20) tab_labels[i] = hui::design::clip_end(cr, tab_labels[i], budget);
    }

    bool active = (i == app.active_tab);

    // Active tab: elevated fill + accent underline (segmented-control language)
    if (active) {
      int r = static_cast<int>(12.0 * zf);
      int m = static_cast<int>(1.0 * zf);
      int l = x + m;
      int t = m;
      int rw = tab_w - m * 2;
      int rh = tab_h - 1 - m * 2;
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.10);
      draw_rounded_rect(cr, l, t, rw, rh, r);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.9);
      draw_rounded_rect(cr, l + 14, t + rh - 3, rw - 28, 2, 1);
      cairo_fill(cr);
    }

    // Drop target glow on tab header (during file drag)
    if (i == app.drop_target_tab_idx && !active) {
      double pulse = 0.14 + 0.06 * std::sin(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count() * 0.006);
      int r = static_cast<int>(12.0 * zf);
      int m = static_cast<int>(1.0 * zf);
      int l = x + m;
      int t = m;
      int rw = tab_w - m * 2;
      int rh = tab_h - 1 - m * 2;
      cairo_new_path(cr);
      cairo_arc(cr, l + r, t + r, r, M_PI, 1.5 * M_PI);
      cairo_arc(cr, l + rw - r, t + r, r, 1.5 * M_PI, 2.0 * M_PI);
      cairo_arc(cr, l + rw - r, t + rh - r, r, 0.0, 0.5 * M_PI);
      cairo_arc(cr, l + r, t + rh - r, r, 0.5 * M_PI, M_PI);
      cairo_close_path(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, pulse);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, pulse + 0.15);
      cairo_set_line_width(cr, 1.5);
      cairo_stroke(cr);
      app.pendingRedraw = true;
    }

    // Label (dimmed if being dragged)
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            active ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    if (dragging && i == drag_sb_idx) {
      cairo_push_group(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, active ? 1.0 : 0.7);
    cairo_move_to(cr, x + pad, tab_h / 2 + static_cast<int>(5.0 * zf));
    cairo_show_text(cr, tab_labels[i].c_str());

    // Close button
    int close_x = x + tab_w - pad - close_icon_sz;
    int close_y = (tab_h - close_icon_sz) / 2;
    // Draw close "×"
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                          app.dots_btn_hover ? 0.85 : 0.5);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, close_x, close_y);
    cairo_line_to(cr, close_x + close_icon_sz, close_y + close_icon_sz);
    cairo_move_to(cr, close_x + close_icon_sz, close_y);
    cairo_line_to(cr, close_x, close_y + close_icon_sz);
    cairo_stroke(cr);

    if (dragging && i == drag_sb_idx) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.35);
    }

    // Store hit rect + register the retained region (single source of
    // truth for input; see ui/hit_registry.hpp).
    app.tab_hits[i].x = x;
    app.tab_hits[i].w = tab_w;
    app.tab_hits[i].close_x = close_x;
    app.hit_main.add(hui::Hit::tab(i), x, y0, tab_w, tab_h);
    if (close_x < x + tab_w)
      app.hit_main.add(hui::Hit::tab_close(i), close_x, y0, x + tab_w - close_x, tab_h);

    x += tab_w;
  }

  // Dragged tab insertion line
  if (dragging) {
    int slot = app.tab_drag_to_visual;
    int line_x;
    if (slot == 0) {
      line_x = app.tab_hits[0].x;
    } else if (slot >= tab_count) {
      line_x = app.tab_hits[tab_count - 1].x + app.tab_hits[tab_count - 1].w;
    } else {
      line_x = app.tab_hits[slot].x;
    }
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, line_x, static_cast<int>(4.0 * zf));
    cairo_line_to(cr, line_x, tab_h - static_cast<int>(4.0 * zf));
    cairo_stroke(cr);
  }

  // Ghost tab following cursor
  if (dragging && drag_sb_idx >= 0 && drag_sb_idx < tab_count) {
    int ghost_x = app.tab_drag_current_x - app.tab_hits[drag_sb_idx].w / 2;
    int ghost_w = app.tab_hits[drag_sb_idx].w;
    int r = static_cast<int>(12.0 * zf);
    int m = static_cast<int>(1.0 * zf);
    cairo_new_path(cr);
    cairo_arc(cr, ghost_x + m + r, m + r, r, M_PI, 1.5 * M_PI);
    cairo_arc(cr, ghost_x + ghost_w - m - r, m + r, r, 1.5 * M_PI, 2.0 * M_PI);
    cairo_arc(cr, ghost_x + ghost_w - m - r, tab_h - 1 - m - r, r, 0.0, 0.5 * M_PI);
    cairo_arc(cr, ghost_x + m + r, tab_h - 1 - m - r, r, 0.5 * M_PI, M_PI);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.5);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
  }
}

void draw_list_view(AppState& app, cairo_t* cr, int content_x,
                    int content_y, int content_w, int view_h) {
  double zf = app.zoom_pct / 100.0;
  int entry_h = app.entry_height;
  int text_x = content_x + static_cast<int>(40.0 * zf);
  int icon_size = static_cast<int>(24.0 * zf);
  auto col_w = [&](bool on, int base) {
    return on ? static_cast<int>(base * zf) : 0;
  };
  int own_w  = col_w(app.col_owner, 90);
  int grp_w  = col_w(app.col_group, 90);
  int prm_w  = col_w(app.col_perms, 84);
  int ext_w  = col_w(app.col_ext, 70);
  int tgt_w  = col_w(app.col_target, 150);
  int extra_total = own_w + grp_w + prm_w + ext_w + tgt_w;
  int name_w = std::max(static_cast<int>(120 * zf),
                        static_cast<int>(content_w * app.col_name_frac) - extra_total);
  int size_w = static_cast<int>(content_w * app.col_size_frac);
  int date_w = static_cast<int>(content_w * app.col_date_frac);
  int name_x = text_x;
  int size_x = name_x + name_w;
  int date_x = size_x + size_w;
  int own_x  = date_x + date_w;
  int grp_x  = own_x + own_w;
  int prm_x  = grp_x + grp_w;
  int ext_x  = prm_x + prm_w;
  int tgt_x  = ext_x + ext_w;
  int type_x = tgt_x + tgt_w;
  int type_w = content_w - (type_x - content_x);

  // ── Column header row ──
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.05);
  cairo_rectangle(cr, content_x, content_y, content_w, entry_h);
  cairo_fill(cr);
  draw_column_header(app, cr, name_x, content_y, name_w, entry_h, "Name",
                      app.col_resizing == 0 || app.col_hover_divider);
  draw_column_header(app, cr, size_x, content_y, size_w, entry_h, "Size",
                      app.col_resizing == 1 || app.col_hover_divider);
  draw_column_header(app, cr, date_x, content_y, date_w, entry_h, "Date",
                      app.col_resizing == 2 || app.col_hover_divider);
  app.hit_main.add(hui::Hit::kHeaderSeg + 0, name_x, content_y, name_w, entry_h);
  app.hit_main.add(hui::Hit::kHeaderSeg + 1, size_x, content_y, size_w, entry_h);
  app.hit_main.add(hui::Hit::kHeaderSeg + 2, date_x, content_y, own_x - date_x, entry_h);
  app.hit_main.add(hui::Hit::kHeaderSeg + 3, date_x + date_w, content_y,
                   content_x + content_w - (date_x + date_w), entry_h);
  app.hit_main.add(hui::Hit::kHeaderDiv + 0, size_x - 4, content_y, 8, entry_h);
  app.hit_main.add(hui::Hit::kHeaderDiv + 1, date_x - 4, content_y, 8, entry_h);
  app.hit_main.add(hui::Hit::kHeaderDiv + 2, date_x + date_w - 4, content_y, 8, entry_h);
  if (app.col_show_type) {
    draw_column_header(app, cr, type_x, content_y, type_w, entry_h, "Type",
                        app.col_resizing == 3 || app.col_hover_divider);
  }
  if (app.col_owner)
    draw_column_header(app, cr, own_x, content_y, own_w, entry_h, "Owner", false);
  if (app.col_group)
    draw_column_header(app, cr, grp_x, content_y, grp_w, entry_h, "Group", false);
  if (app.col_perms)
    draw_column_header(app, cr, prm_x, content_y, prm_w, entry_h, "Perms", false);
  if (app.col_ext)
    draw_column_header(app, cr, ext_x, content_y, ext_w, entry_h, "Ext", false);
  if (app.col_target)
    draw_column_header(app, cr, tgt_x, content_y, tgt_w, entry_h, "Link Target", false);

  int y = content_y + entry_h - app.cur_tab().scroll_px;

  std::string prev_group;
  int header_h = static_cast<int>(entry_h * 0.55);
  std::unordered_map<std::string, int> header_ys;
  bool sticky_recorded = false;
  std::string first_vis_label;
  int first_vis_header_y = INT_MIN;

  for (int vi = 0; vi < static_cast<int>(app.cur_tab().visible_entries.size()); ++vi) {
    int real_idx = app.cur_tab().visible_entries[vi];
    if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
      continue;
    auto& entry = app.cur_tab().entries[real_idx];

    std::string row_label;
    if (app.cur_tab().group_field > 0) {
      row_label = group_label_for(app, entry);
      if (row_label != prev_group) {
        prev_group = row_label;
        header_ys[row_label] = y;
        if (y + header_h >= content_y)
          draw_group_header_band(app, cr, content_x, y, content_w, header_h, row_label);
        y += header_h;
      }
      if (!sticky_recorded && y + entry_h >= content_y) {
        sticky_recorded = true;
        first_vis_label = row_label;
        auto it = header_ys.find(row_label);
        first_vis_header_y = (it != header_ys.end()) ? it->second : INT_MIN;
      }
    }

    app.hit_main.add(hui::Hit::view_row(vi), content_x, y, content_w, entry_h);

    if (y + entry_h < content_y) { y += entry_h; continue; }
    if (y > content_y + view_h) break;

    bool selected =
        vi == app.cur_tab().selected_idx ||
        std::find(app.cur_tab().multi_selected.begin(), app.cur_tab().multi_selected.end(), vi) !=
            app.cur_tab().multi_selected.end();
    bool hovered = vi == app.cur_tab().hover_idx;
    bool drop_target = !app.drop_target_path.empty() && !app.drop_target_is_sidebar &&
                       vi == app.drop_target_idx && entry.is_dir;
    bool is_cut = !app.cut_paths.empty() && app.cut_paths.count(entry.path);

    // M3 active indicator: accent pill, inset with rounded ends.
    if (selected) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                             0.16);
      draw_rounded_rect(cr, content_x + 4, y + 2, content_w - 8, entry_h - 4,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
    } else if (drop_target) {
      double pulse = 0.18 + 0.07 * std::sin(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count() * 0.006);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, pulse);
      draw_rounded_rect(cr, content_x + 4, y + 2, content_w - 8, entry_h - 4,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
      app.pendingRedraw = true;
    } else if (hovered) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, content_x + 4, y + 2, content_w - 8, entry_h - 4,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
    }

    // Cut indicator: dashed border on cut files
    if (is_cut) {
      cairo_save(cr);
      double dash_len = 5.0;
      double gap_len = 3.0;
      cairo_set_dash(cr, &dash_len, 1, 0.0);
      cairo_set_line_width(cr, 1.5);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                             app.text_secondary_b, 0.6);
      draw_rounded_rect(cr, content_x + 1, y + 1, content_w - 2, entry_h - 2,
                        static_cast<int>(4.0 * zf));
      cairo_stroke(cr);
      cairo_set_dash(cr, &dash_len, 0, 0.0);
      cairo_restore(cr);
    }

    bool hidden = entry.is_hidden;
    if (hidden || is_cut) cairo_push_group(cr);

    cairo_surface_t* thumb = nullptr;
    if (entry.type == FileType::Image || entry.type == FileType::Video) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    } else if (entry.type == FileType::Document && (is_pdf_extension(entry.path) || is_epub_extension(entry.path))) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    }
    draw_file_icon_cairo(app, cr, content_x + 8, y + (entry_h - icon_size) / 2,
                          icon_size, entry.type, selected, entry.icon_name, thumb, &entry.path, &entry);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0 * zf);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, text_x, y + entry_h / 2 + 4);

    std::string display_name = hui::design::clip_keep_ext(cr, entry.name, name_w - 20);
    cairo_show_text(cr, display_name.c_str());

    if (!entry.is_dir) {
      cairo_set_font_size(cr, 12.0 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, 1.0);
      cairo_move_to(cr, size_x, y + entry_h / 2 + 4);
      cairo_show_text(cr, format_size(entry.size).c_str());
    }

    cairo_set_font_size(cr, 12.0 * zf);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 1.0);
    cairo_move_to(cr, date_x, y + entry_h / 2 + 4);
    struct tm tm_buf;
    struct tm* lt = localtime_r(&entry.modified_sec, &tm_buf);
    if (lt) {
      char date_buf[32];
      strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", lt);
      cairo_show_text(cr, date_buf);
    }

    // Optional stat-based columns
    if (extra_total > 0) {
      cairo_set_font_size(cr, 12.0 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                             app.text_secondary_b, 1.0);
      auto clip_show = [&](int cx0, int cw0, const std::string& s) {
        if (cw0 <= 0 || s.empty()) return;
        cairo_save(cr);
        cairo_rectangle(cr, cx0, y, cw0 - 6, entry_h);
        cairo_clip(cr);
        cairo_move_to(cr, cx0, y + entry_h / 2 + 4);
        cairo_show_text(cr, s.c_str());
        cairo_restore(cr);
      };
      clip_show(own_x, own_w, entry.owner);
      clip_show(grp_x, grp_w, entry.group);
      clip_show(prm_x, prm_w,
                entry.mode ? format_mode(entry.mode) : std::string());
      clip_show(ext_x, ext_w, entry.extension);
      clip_show(tgt_x, tgt_w,
                entry.link_target.empty() && !entry.is_dir
                    ? std::string()
                    : entry.link_target);
    }

    if (hidden) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.5);
    } else if (is_cut) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.5);
    }

    y += entry_h;
  }

  // Sticky group header pinned to the top of the viewport
  if (app.cur_tab().group_field > 0 && sticky_recorded &&
      first_vis_header_y != INT_MIN && first_vis_header_y < content_y)
    draw_group_header_band(app, cr, content_x, content_y, content_w, header_h,
                            first_vis_label, true);

  app.cur_tab().content_h = y - content_y + app.cur_tab().scroll_px - entry_h;
}



// ── grid view ────────────────────────────────────────────────────

// Per-frame Pango shaping/layout is the dominant non-pixman cost in grid
// view (harfbuzz+pango+layout allocations). The same ~N visible labels
// repeat every frame, so rasterize each label once (already in its theme
// color, premultiplied ARGB) and re-blit it 1:1 per frame. Baking the
// color in (instead of mask-tinting white each frame) lets the per-frame
// blit use pixman's src-over fast path rather than a 3-surface mask
// composite. Keyed on the exact shaping inputs + quantized color so theme
// switches naturally start a new working set (old keys LRU out).
struct LabelRasterKey {
  std::string text; // final (possibly truncated) label
  int width;        // layout width in px
  int font;         // absolute font px
  uint32_t color;   // 0x00RRGGBB — quantized theme text color
  bool operator==(const LabelRasterKey& o) const {
    return text == o.text && width == o.width && font == o.font && color == o.color;
  }
};
struct LabelRasterKeyHash {
  std::size_t operator()(const LabelRasterKey& k) const {
    std::size_t h = std::hash<std::string>{}(k.text);
    h ^= static_cast<std::size_t>(k.width) << 8;
    h ^= static_cast<std::size_t>(k.font) << 20;
    h ^= static_cast<std::size_t>(k.color) << 30;
    return h;
  }
};
struct LabelRasterCache {
  std::list<std::pair<LabelRasterKey, cairo_surface_t*>> mru;
  std::unordered_map<LabelRasterKey, decltype(mru)::iterator, LabelRasterKeyHash> map;
  static constexpr int kMax = 1024;
  void clear() {
    for (auto& kv : mru) cairo_surface_destroy(kv.second);
    mru.clear();
    map.clear();
  }
  // Rasterize (or fetch) a white-alpha label surface for text at the given
  // wrap width (px) and font size (px). lines_px gives the surface height
  // = 2 lines. Font options are copied from cr so metrics match the window.
  // Truncation to 2 lines happens on miss, exactly as draw_grid_view did.
  cairo_surface_t* lookup(const LabelRasterKey& key, cairo_t* cr, int lines_px) {
    auto it = map.find(key);
    if (it != map.end()) { mru.splice(mru.begin(), mru, it->second); return it->second->second; }
    // A 2-line label is taller than the line_h grid math assumes (Pango
    // line height ~18px at 13px font vs the 16px box), and the inline draw
    // never clipped text to the box. Measure the real per-line height so
    // the raster surface doesn't clip descenders, matching the old output.
    int h;
    {
      cairo_surface_t* probe_s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t* probe = cairo_create(probe_s);
      if (cr) {
        cairo_font_options_t* fo = cairo_font_options_create();
        cairo_get_font_options(cr, fo);
        cairo_set_font_options(probe, fo);
        cairo_font_options_destroy(fo);
      }
      PangoFontDescription* mdesc = pango_font_description_new();
      pango_font_description_set_family(mdesc, "Sans");
      pango_font_description_set_absolute_size(mdesc, key.font * PANGO_SCALE);
      PangoLayout* probe_pl = pango_cairo_create_layout(probe);
      PangoFontMetrics* mm = pango_context_get_metrics(pango_layout_get_context(probe_pl), mdesc, NULL);
      int line_h_real = pango_font_metrics_get_height(mm) / PANGO_SCALE;
      if (line_h_real < 1) line_h_real = 1;
      pango_font_metrics_unref(mm);
      pango_font_description_free(mdesc);
      g_object_unref(probe_pl);
      h = std::max(lines_px + 2, 2 * line_h_real + 2);
      cairo_surface_destroy(probe_s);
      cairo_destroy(probe);
    }
    cairo_surface_t* surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, key.width, h);
    cairo_t* lc = cairo_create(surf);
    cairo_set_source_rgba(lc, (key.color >> 16 & 0xFF) / 255.0,
                          (key.color >> 8 & 0xFF) / 255.0,
                          (key.color & 0xFF) / 255.0, 1.0);
    PangoLayout* pl = pango_cairo_create_layout(lc);
    auto* desc = pango_font_description_new();
    pango_font_description_set_family(desc, "Sans");
    pango_font_description_set_absolute_size(desc, key.font * PANGO_SCALE);
    pango_layout_set_font_description(pl, desc);
    pango_layout_set_width(pl, key.width * PANGO_SCALE);
    pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_NONE);

    std::string label = key.text;
    pango_layout_set_text(pl, label.c_str(), -1);
    if (pango_layout_get_line_count(pl) > 2) {
      std::string ext;
      auto dot = label.rfind('.');
      if (dot != std::string::npos && dot > 0) {
        ext = label.substr(dot);
        label = label.substr(0, dot);
      }
      while (!label.empty()) {
        std::string candidate = ext.empty() ? (label + "...") : (label + "..." + ext);
        pango_layout_set_text(pl, candidate.c_str(), -1);
        if (pango_layout_get_line_count(pl) <= 2) break;
        label.pop_back();
      }
      if (label.empty()) {
        pango_layout_set_text(pl, (ext.empty() ? "..." : ("..." + ext)).c_str(), -1);
      }
    }
    pango_cairo_show_layout(lc, pl);
    pango_font_description_free(desc);
    g_object_unref(pl);
    cairo_destroy(lc);
    mru.push_front({key, surf});
    map.emplace(key, mru.begin());
    if (static_cast<int>(mru.size()) > kMax) {
      cairo_surface_destroy(mru.back().second);
      map.erase(mru.back().first);
      mru.pop_back();
    }
    return surf;
  }
};
static LabelRasterCache g_grid_labels;

// ── TEMP grid profiler ───────────────────────────────────────────
struct DrawZone {
  const char* name;
  std::chrono::steady_clock::time_point t0;
  AppState* app;
  bool done = false;
  DrawZone(const char* n, AppState* a)
      : name(n), t0(std::chrono::steady_clock::now()), app(a) {}
  void end() {
    if (done) return;
    done = true;
    if (app && eh::trace::enabled().load(std::memory_order_relaxed)) {
      double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
      app->resize_phase_samples.emplace_back(name, ms);
    }
  }
  ~DrawZone() { end(); }
};
// ── end TEMP grid profiler ───────────────────────────────────────

// Bench-only micro-profiler. Zero-cost when app.paint_profile is false:
// the active check happens before either clock() call. `enabled` further
// restricts which cells are timed (e.g. only hidden/cut ones).
struct PaintStopwatch {
  AppState* app;
  std::uint64_t* dst;
  std::chrono::steady_clock::time_point t0;
  bool active;
  PaintStopwatch(AppState& a, std::uint64_t& counter, bool enabled = true)
      : app(&a), dst(&counter), active(a.paint_profile && enabled) {
    if (active) t0 = std::chrono::steady_clock::now();
  }
  ~PaintStopwatch() {
    if (active)
      *dst += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - t0).count());
  }
};

void draw_grid_view(AppState& app, cairo_t* cr, int content_x,
                    int content_y, int content_w, int view_h, int strip_y0,
                    int strip_y1) {
  if (strip_y1 < 0) { strip_y0 = content_y; strip_y1 = content_y + view_h; }
  double zf = app.zoom_pct / 100.0;

  // Grid: auto-fill, minmax(110px, 1fr). Geometry now lives in exactly one
  // place — layout.cpp's compute_grid_layout() — so draw, hit-test, and
  // keyboard nav always agree on the same row_h/cols.
  GridLayout gl = compute_grid_layout(content_w, zf);
  int min_cell_w = gl.min_cell_w;
  int col_gap = gl.col_gap;
  int row_gap = gl.row_gap;
  int cols = gl.cols;
  int cell_w = gl.cell_w;
  app.grid_cell_size = cell_w;
  app.grid_cols = cols;
  app.grid_cell_gap = col_gap;

  int icon_size = gl.icon_size;
  int icon_area = gl.icon_area;
  int label_h = gl.label_h;
  int text_gap = gl.text_gap;
  int item_h = gl.item_h;
  int row_h = gl.row_h;
  app.grid_row_h = row_h;

  int grid_w = gl.grid_w;
  int grid_offset_x = gl.grid_offset_x;

  int y = content_y + row_gap - app.cur_tab().scroll_px;

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * zf);

  int group_extra = 0;
  std::string prev_group;
  int gheader_h = static_cast<int>(22.0 * zf);
  std::unordered_map<std::string, int> gheader_ys;
  bool sticky_recorded = false;
  std::string first_vis_label;
  int first_vis_header_y = INT_MIN;

  // Batch the exact-fit icon + label blits into one raw-pixel composite pass
  // (skips cairo/pixman per-op overhead). Active only when the destination is
  // a plain ARGB32 image surface; otherwise every blit falls back to cairo.
  BatchedBlitter grid_blit;
  const bool strip_mode =
      (strip_y0 != content_y || strip_y1 != content_y + view_h);
  if (strip_mode) {
    // Strip pass (scroll-delta reuse): limit every cell blit AND the cairo
    // fallback drawing to the exposed band. Rows above band_y0 already carry
    // the correct post-shift pixels; re-blending a band-straddling cell there
    // would double-blend its translucent icon/label over them.
    grid_blit.attach(cr, content_x, strip_y0, content_w, strip_y1 - strip_y0);
    cairo_save(cr);
    cairo_rectangle(cr, content_x, strip_y0, content_w, strip_y1 - strip_y0);
    cairo_clip(cr);
  } else {
    grid_blit.attach(cr, content_x, content_y, content_w, view_h);
  }

  for (int vi = 0; vi < static_cast<int>(app.cur_tab().visible_entries.size()); ++vi) {
    int col = vi % cols;
    int row = vi / cols;

    // Group By: new band at each row where the group changes
    if (app.cur_tab().group_field > 0 && col == 0) {
      int gri = app.cur_tab().visible_entries[vi];
      if (gri >= 0 && gri < static_cast<int>(app.cur_tab().entries.size())) {
        std::string label = group_label_for(app, app.cur_tab().entries[gri]);
        if (label != prev_group) {
          prev_group = label;
          int hy = y + row * row_h + group_extra;
          gheader_ys[label] = hy;
          if (hy + gheader_h >= content_y &&
              hy + gheader_h > strip_y0 && hy < strip_y1) {
            PaintStopwatch sw_header(app, app.profile_grid_header_ns);
            draw_group_header_band(app, cr, content_x, hy, content_w,
                                    gheader_h, label);
          }
          group_extra += gheader_h;
        }
        if (!sticky_recorded &&
            y + row * row_h + group_extra + item_h >= content_y) {
          sticky_recorded = true;
          first_vis_label = label;
          auto it = gheader_ys.find(label);
          first_vis_header_y = (it != gheader_ys.end()) ? it->second : INT_MIN;
        }
      }
    }

    int cx = content_x + grid_offset_x + col * (cell_w + col_gap);
    int cy = y + row * row_h + group_extra;

    app.hit_main.add(hui::Hit::view_row(vi), cx, cy, cell_w, item_h);

    if (cy + item_h < content_y) continue;
    if (cy > content_y + view_h) break;
    // Strip mode (scroll-delta reuse): draw only band-intersecting cells.
    if (cy + item_h <= strip_y0 || cy >= strip_y1) continue;

    int real_idx = app.cur_tab().visible_entries[vi];
    if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
      continue;
    auto& entry = app.cur_tab().entries[real_idx];

    bool selected =
        vi == app.cur_tab().selected_idx ||
        std::find(app.cur_tab().multi_selected.begin(), app.cur_tab().multi_selected.end(), vi) !=
            app.cur_tab().multi_selected.end();
    bool hovered = vi == app.cur_tab().hover_idx;
    bool drop_target = !app.drop_target_path.empty() && !app.drop_target_is_sidebar &&
                       vi == app.drop_target_idx && entry.is_dir;
    bool is_cut = !app.cut_paths.empty() && app.cut_paths.count(entry.path);

    bool hidden = entry.is_hidden;
    PaintStopwatch sw_hidden(app, app.profile_grid_hidden_ns, hidden || is_cut);
    if (hidden || is_cut) cairo_push_group(cr);

    int bg_x = cx + (cell_w - icon_size) / 2;
    int bg_y = cy;

    {
      PaintStopwatch sw_stroke(app, app.profile_grid_stroke_ns);
      // Selection/hover overlay spans the whole item (icon + label) with the
      // same 2px inset top AND bottom, so a wrapped long name is fully framed
      // and never pokes past the rounded edge. The label box now holds the
      // real 2-line height (layout.cpp's measured line_h), which makes the
      // bottom inset actually reach the text.
      const int overlay_h = item_h - 4; // 2px inset on both sides
      if (selected) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
        draw_rounded_rect(cr, cx + 4, cy + 2, cell_w - 8, overlay_h,
                          static_cast<int>(8.0 * zf));
        cairo_fill(cr);
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.6);
        cairo_set_line_width(cr, 1.5);
        draw_rounded_rect(cr, cx + 4, cy + 2, cell_w - 8, overlay_h,
                          static_cast<int>(8.0 * zf));
        cairo_stroke(cr);
      } else if (drop_target) {
        double pulse = 0.30 + 0.10 * std::sin(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() * 0.006);
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, pulse);
        cairo_set_line_width(cr, 2.0);
        draw_rounded_rect(cr, bg_x + 1, bg_y + 1, icon_size - 2, icon_size - 2,
                          static_cast<int>(8.0 * zf));
        cairo_stroke(cr);
        app.pendingRedraw = true;
      } else if (hovered) {
        // Cover the whole cell (icon + label), not just the icon.
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
        draw_rounded_rect(cr, cx + 4, cy + 2, cell_w - 8, overlay_h,
                          static_cast<int>(8.0 * zf));
        cairo_fill(cr);
      }

      // Cut indicator: dashed border on cut files
      if (is_cut) {
        cairo_save(cr);
        double dash_len = 5.0;
        double gap_len = 3.0;
        cairo_set_dash(cr, &dash_len, 1, 0.0);
        cairo_set_line_width(cr, 1.5);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                               app.text_secondary_b, 0.6);
        draw_rounded_rect(cr, bg_x + 1, bg_y + 1, icon_size - 2, icon_size - 2,
                          static_cast<int>(8.0 * zf));
        cairo_stroke(cr);
        cairo_set_dash(cr, &dash_len, 0, 0.0);
        cairo_restore(cr);
      }
    }

    // File icon
    cairo_surface_t* thumb = nullptr;
    if (entry.type == FileType::Image || entry.type == FileType::Video) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    } else if (entry.type == FileType::Document && (is_pdf_extension(entry.path) || is_epub_extension(entry.path))) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    }
    {
      PaintStopwatch sw_icon(app, app.profile_grid_icon_ns);
      draw_file_icon_cairo(app, cr, bg_x, bg_y,
                            icon_size, entry.type, selected, entry.icon_name, thumb, &entry.path, &entry,
                            (hidden || is_cut) ? nullptr : &grid_blit);
    }

    // Label (word/char-wrapped to 2 lines; extension preserved if truncation
    // is needed; top-anchored so spacing is consistent regardless of 1 vs 2 lines).
    // The visible cell count (~50) is far smaller than the entry count
    // (50k); rasterizing each visible label once and mask-blitting 1:1 per
    // frame removes the per-frame shaping/alloc churn from the hot path.
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    int layout_w = cell_w - 8;
    int font_px = static_cast<int>(13.0 * zf);
    int line_h = gl.line_h; // measured per-line height (layout.cpp)
    uint32_t color = (static_cast<uint32_t>(app.text_r * 255) << 16) |
                     (static_cast<uint32_t>(app.text_g * 255) << 8) |
                     static_cast<uint32_t>(app.text_b * 255);
    cairo_surface_t* label_surf = g_grid_labels.lookup(
        LabelRasterKey{entry.name, layout_w, font_px, color}, cr, 2 * line_h);

    int label_area_h = label_h;
    int label_x = cx + (cell_w - layout_w) / 2;
    int label_y = cy + icon_size + text_gap + std::max(0, (label_area_h - 2 * line_h) / 2);
    {
      PaintStopwatch sw_label(app, app.profile_grid_label_ns);
      if (!(hidden || is_cut) && grid_blit.active()) {
        grid_blit.add(label_surf, label_x, label_y);
      } else {
        cairo_set_source_surface(cr, label_surf, label_x, label_y);
        cairo_paint(cr);
      }
    }

    if (hidden || is_cut) {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.5);
    }
  }

  if (strip_mode) cairo_restore(cr);

  // Composite the batched icon/label blits before the sticky header draws on
  // top (matches the old inline ordering: cells first, header over them).
  {
    PaintStopwatch sw_flush(app, app.profile_grid_flush_ns);
    grid_blit.flush();
  }

  int rows = (static_cast<int>(app.cur_tab().visible_entries.size()) + cols - 1) / cols;
  app.cur_tab().content_h = y + rows * row_h + group_extra - content_y + app.cur_tab().scroll_px + row_gap;

  // Sticky group header pinned to the top of the viewport
  if (app.cur_tab().group_field > 0 && sticky_recorded &&
      first_vis_header_y != INT_MIN && first_vis_header_y < content_y) {
    PaintStopwatch sw_header(app, app.profile_grid_header_ns);
    draw_group_header_band(app, cr, content_x, content_y, content_w,
                            gheader_h, first_vis_label, true);
  }
}

// ── Build tree view entries ──────────────────────────────────────
void build_tree_entries(AppState& app) {
  auto& tab = app.cur_tab();
  tab.tree_entries.clear();
  tab.tree_entries.reserve(tab.visible_entries.size());

  // Recursive expansion for tree rows. `upper` carries the branch-guide
  // state of ancestor columns into this directory's children: for each
  // column k, FULL means the ancestor's spine continues through this
  // subtree, NONE means it already terminated at that ancestor's elbow.
  constexpr unsigned char kGuideNone = 0, kGuideFull = 1, kGuideLast = 2;
  auto collect = [&](auto&& self, const fs::path& dir, int depth,
                     const std::vector<unsigned char>& upper) -> void {
    std::vector<TreeEntry> children;
    std::error_code ec;
    const auto hidden_names = read_hidden_file(dir.string());
    for (auto& de : fs::directory_iterator(dir, ec)) {
      auto path = de.path();
      auto name = path.filename().string();
      if (name.empty()) continue;
      if (!app.show_hidden &&
          (name[0] == '.' || hidden_names.count(name) > 0)) continue;
      bool is_dir = de.is_directory(ec);
      bool child_expanded = tab.tree_expanded.count(path.string()) > 0;
      TreeEntry child;
      child.name = name;
      child.path = path.string();
      child.is_dir = is_dir;
      child.depth = depth;
      child.has_children = is_dir;
      child.is_expanded = child_expanded;
      child.type = detect_file_type_for_path(name, is_dir, path.string());
      child.guides = upper;
      child.guides.push_back(0); // filled in below once siblings are known
      children.push_back(std::move(child));
    }
    std::sort(children.begin(), children.end(), [](auto& a, auto& b) {
      if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
      return strverscmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    for (size_t j = 0; j < children.size(); ++j) {
      const bool last = (j + 1 == children.size());
      auto& child = children[j];
      // This row's own column: ├ (spine passes on) or └ (terminates here).
      child.guides.back() = last ? kGuideLast : kGuideFull;
      TreeEntry pushed = child; // keep guides/type for the recursion below
      if (child.is_dir && child.is_expanded && depth < 2) {
        // Ancestor columns below this row: FULL only where the spine
        // continues past it; a terminated (kGuideLast) column goes dark.
        std::vector<unsigned char> sub_upper;
        sub_upper.reserve(child.guides.size());
        for (unsigned char g : child.guides)
          sub_upper.push_back(g == kGuideLast ? kGuideNone : g);
        self(self, fs::path(child.path), depth + 1, sub_upper);
      }
      tab.tree_entries.push_back(std::move(child));
    }
  };

  for (int vi : tab.visible_entries) {
    auto& entry = tab.entries[vi];
    bool is_expanded = tab.tree_expanded.count(entry.path) > 0;
    bool has_children = entry.is_dir;
    TreeEntry te;
    te.name = entry.name;
    te.path = entry.path;
    te.is_dir = entry.is_dir;
    te.depth = 0;
    te.has_children = has_children;
    te.is_expanded = is_expanded;
    te.type = entry.type;
    tab.tree_entries.push_back(std::move(te));
    if (entry.is_dir && is_expanded) {
      collect(collect, fs::path(entry.path), 1, {});
    }
  }
  tab.tree_entries_dirty = false;
}

// ── Tree view ────────────────────────────────────────────────────
void draw_tree_view(AppState& app, cairo_t* cr, int content_x,
                    int content_y, int content_w, int view_h) {
  double zf = app.zoom_pct / 100.0;
  int entry_h = static_cast<int>(28.0 * zf);
  int icon_size = static_cast<int>(20.0 * zf);
  int indent_step = static_cast<int>(24.0 * zf);
  int arrow_w = static_cast<int>(16.0 * zf);

  if (app.cur_tab().tree_entries_dirty) build_tree_entries(app);

  int y = content_y - app.cur_tab().scroll_px;

  for (int vi = 0; vi < static_cast<int>(app.cur_tab().tree_entries.size()); ++vi) {
    auto& te = app.cur_tab().tree_entries[vi];
    int indent = te.depth * indent_step;

    app.hit_main.add(hui::Hit::view_row(vi), content_x, y, content_w, entry_h);
    app.hit_main.add(hui::Hit::view_arrow(vi), content_x + indent + 4, y + (entry_h - arrow_w) / 2,
                     arrow_w, arrow_w);

    if (y + entry_h < content_y) { y += entry_h; continue; }
    if (y > content_y + view_h) break;

    // Selection: selected_idx is a TREE-row index while in this view
    // (keyboard handlers treat it that way too); also honor the explicit
    // path so right-clicked rows stay highlighted.
    bool selected = vi == app.cur_tab().selected_idx ||
                    te.path == app.cur_tab().tree_selected_path;
    bool hovered = vi == app.cur_tab().hover_idx;

    // M3 active indicator: accent pill, inset with rounded ends.
    if (selected) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      draw_rounded_rect(cr, content_x + 4, y + 2, content_w - 8, entry_h - 4, 8);
      cairo_fill(cr);
    } else if (hovered) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, content_x + 4, y + 2, content_w - 8, entry_h - 4, 8);
      cairo_fill(cr);
    }
    // Branch guide lines: a faint spine in
    // each ancestor's expander column, elbowing into this row. kGuideFull
    // passes through (├), kGuideLast terminates at the midline (└).
    if (!te.guides.empty()) {
      const double row_top = static_cast<double>(y);
      const double row_mid = y + entry_h / 2.0;
      const double row_bot = y + entry_h;
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.16);
      cairo_set_line_width(cr, 1.0);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      for (size_t k = 0; k < te.guides.size(); ++k) {
        const double col_cx =
            content_x + static_cast<double>(k) * indent_step + 4 + arrow_w * 0.5;
        switch (te.guides[k]) {
          case 1:  // full vertical through the row
            cairo_move_to(cr, col_cx, row_top);
            cairo_line_to(cr, col_cx, row_bot);
            cairo_stroke(cr);
            break;
          case 2:  // last child: vertical stops at the elbow
            cairo_move_to(cr, col_cx, row_top);
            cairo_line_to(cr, col_cx, row_mid);
            cairo_stroke(cr);
            break;
          default:
            break;
        }
      }
      // Elbow from the nearest ancestor column across to the file icon.
      const double last_col_cx =
          content_x + static_cast<double>(te.guides.size() - 1) * indent_step +
          4 + arrow_w * 0.5;
      const int elbow_icon_x =
          content_x + te.depth * indent_step + arrow_w + 4;
      cairo_move_to(cr, last_col_cx, row_mid);
      cairo_line_to(cr, elbow_icon_x - 5.0 * zf, row_mid);
      cairo_stroke(cr);
    }

    // Expand/collapse chevron for directories.
    // A thin stroked
    // chevron (pan-end/pan-down symbolic style) that rotates 90° between
    // states, dimmed at rest and brightening to full when the row is hot.
    // Pure cairo geometry, so it can't blank out on missing font glyphs the
    // way the old ▼/▶ text approach did.
    int arrow_x = content_x + indent + 4;
    int arrow_y = y + (entry_h - arrow_w) / 2;
    if (te.is_dir) {
      const bool hot = hovered || selected;
      const double cx = arrow_x + arrow_w * 0.5;
      const double cy = arrow_y + arrow_w * 0.5;
      const double r  = arrow_w * 0.26;   // chevron half-span
      const double dip = r * 1.05;        // apex depth below the arms
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                            hot ? 1.0 : 0.68);
      cairo_save(cr);
      cairo_translate(cr, cx, cy);
      if (!te.is_expanded) cairo_rotate(cr, -M_PI / 2.0);
      cairo_set_line_width(cr, std::max(1.25, arrow_w * 0.115));
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      cairo_move_to(cr, -r, -dip * 0.5);
      cairo_line_to(cr, 0.0, dip * 0.55);
      cairo_line_to(cr, r, -dip * 0.5);
      cairo_stroke(cr);
      cairo_restore(cr);
    }

    // File icon
    int icon_x = content_x + indent + arrow_w + 4;
    int icon_y = y + (entry_h - icon_size) / 2;
    FileType ftype = te.is_dir ? FileType::Folder : te.type;
    draw_file_icon_cairo(app, cr, icon_x, icon_y, icon_size, ftype, selected, "", nullptr, &te.path);

    // Name
    int text_x = icon_x + icon_size + 6;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0 * zf);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, text_x, y + entry_h / 2 + 4);
    std::string display = hui::design::clip_end(cr, te.name, content_w - (text_x - content_x) - 10);
    cairo_show_text(cr, display.c_str());

    y += entry_h;
  }

  app.cur_tab().content_h = y - content_y + app.cur_tab().scroll_px;
}

// ── Compact view ─────────────────────────────────────────────────
void draw_compact_view(AppState& app, cairo_t* cr, int content_x,
                       int content_y, int content_w, int view_h) {
  double zf = app.zoom_pct / 100.0;
  int entry_h = static_cast<int>(24.0 * zf);
  int icon_size = static_cast<int>(16.0 * zf);
  int text_x = content_x + static_cast<int>(28.0 * zf);

  int y = content_y - app.cur_tab().scroll_px;

  std::string prev_group;
  int cheader_h = static_cast<int>(20.0 * zf);
  std::unordered_map<std::string, int> cheader_ys;
  bool sticky_recorded = false;
  std::string first_vis_label;
  int first_vis_header_y = INT_MIN;

  for (int vi = 0; vi < static_cast<int>(app.cur_tab().visible_entries.size()); ++vi) {
    int real_idx = app.cur_tab().visible_entries[vi];
    if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
      continue;
    auto& entry = app.cur_tab().entries[real_idx];

    std::string row_label;
    if (app.cur_tab().group_field > 0) {
      row_label = group_label_for(app, entry);
      if (row_label != prev_group) {
        prev_group = row_label;
        cheader_ys[row_label] = y;
        if (y + cheader_h >= content_y)
          draw_group_header_band(app, cr, content_x, y, content_w, cheader_h,
                                  row_label);
        y += cheader_h;
      }
      if (!sticky_recorded && y + entry_h >= content_y) {
        sticky_recorded = true;
        first_vis_label = row_label;
        auto it = cheader_ys.find(row_label);
        first_vis_header_y = (it != cheader_ys.end()) ? it->second : INT_MIN;
      }
    }

    app.hit_main.add(hui::Hit::view_row(vi), content_x, y, content_w, entry_h);

    if (y + entry_h < content_y) { y += entry_h; continue; }
    if (y > content_y + view_h) break;

    bool selected = vi == app.cur_tab().selected_idx ||
                    std::find(app.cur_tab().multi_selected.begin(),
                              app.cur_tab().multi_selected.end(), vi)
                        != app.cur_tab().multi_selected.end();
    bool hovered = vi == app.cur_tab().hover_idx;

    // M3 active indicator: accent pill, inset with rounded ends.
    if (selected) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      draw_rounded_rect(cr, content_x + 4, y + 2, content_w - 8, entry_h - 4,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
    } else if (hovered) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
      draw_rounded_rect(cr, content_x + 4, y + 2, content_w - 8, entry_h - 4,
                        static_cast<int>(8.0 * zf));
      cairo_fill(cr);
    }

    cairo_surface_t* thumb = nullptr;
    if (entry.type == FileType::Image || entry.type == FileType::Video) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    } else if (entry.type == FileType::Document && (is_pdf_extension(entry.path) || is_epub_extension(entry.path))) {
      thumb = get_thumbnail_lazy(app, vi, entry.path, icon_size);
    }
    draw_file_icon_cairo(app, cr, content_x + 6, y + (entry_h - icon_size) / 2,
                          icon_size, entry.type, selected, entry.icon_name, thumb, &entry.path, &entry);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.0 * zf);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, text_x, y + entry_h / 2 + 4);

    std::string display = hui::design::clip_end(cr, entry.name, content_w - (text_x - content_x) - 6);
    cairo_move_to(cr, text_x, y + entry_h / 2 + 4);
    cairo_show_text(cr, display.c_str());

    y += entry_h;
  }

  // Sticky group header pinned to the top of the viewport
  if (app.cur_tab().group_field > 0 && sticky_recorded &&
      first_vis_header_y != INT_MIN && first_vis_header_y < content_y)
    draw_group_header_band(app, cr, content_x, content_y, content_w,
                            cheader_h, first_vis_label, true);

  app.cur_tab().content_h = y - content_y + app.cur_tab().scroll_px;
}

} // namespace eh::file_browser

