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

// ── status bar ───────────────────────────────────────────────────

void draw_status_bar(AppState& app, cairo_t* cr, int w, int h,
                     int status_h) {
  // Check operation status expiry
  if (!app.operation_status.empty() && app.operation_status_expires_ms > 0) {
    auto now = std::chrono::steady_clock::now();
    auto expiry = std::chrono::steady_clock::time_point(
        std::chrono::milliseconds(app.operation_status_expires_ms));
    if (now >= expiry) {
      app.operation_status.clear();
      app.operation_status_expires_ms = 0;
    }
  }

  double zf = app.zoom_pct / 100.0;
  int y = h - status_h;
  double sa = app.statusbar_opacity_pct / 100.0;

  // bg-zinc-900 style background
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, sa);
  cairo_rectangle(cr, 0, y, w, status_h);
  cairo_fill(cr);

  // border-t zinc-700
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  cairo_rectangle(cr, 0, y, w, 1);
  cairo_fill(cr);

  // px-6 = 24px padding
  int pad = static_cast<int>(24.0 * zf);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * zf); // text-sm
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);

  char status_buf[256];
  size_t sel_count = app.cur_tab().multi_selected.size();

  if (sel_count == 1) {
    int sel = app.cur_tab().multi_selected[0];
    int real_idx = (sel >= 0 && sel < static_cast<int>(app.cur_tab().visible_entries.size()))
                       ? app.cur_tab().visible_entries[sel]
                       : -1;
    if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size())) {
      auto& entry = app.cur_tab().entries[real_idx];
      if (entry.is_dir) {
        // Never walk trees on the paint thread: consult the background
        // dir-stats cache and request a refresh when stale/missing.
        struct ::stat dst{};
        int64_t dmtime = (::stat(entry.path.c_str(), &dst) == 0)
                             ? static_cast<int64_t>(dst.st_mtime) : 0;
        eh::file_browser::dir_stats_request(app, entry.path, dmtime);
        auto ds = app.dir_stat_cache.find(entry.path);
        if (ds != app.dir_stat_cache.end() && ds->second.mtime_sec == dmtime &&
            !ds->second.truncated) {
          std::snprintf(status_buf, sizeof(status_buf),
                        "%s/ \u2014 %llu items (%s)", entry.name.c_str(),
                        (unsigned long long)ds->second.count,
                        format_size(ds->second.bytes).c_str());
        } else if (ds != app.dir_stat_cache.end() &&
                   ds->second.mtime_sec == dmtime && ds->second.truncated) {
          std::snprintf(status_buf, sizeof(status_buf),
                        "%s/ \u2014 %llu+ items", entry.name.c_str(),
                        (unsigned long long)ds->second.count);
        } else {
          std::snprintf(status_buf, sizeof(status_buf), "%s/",
                        entry.name.c_str());
        }
      } else {
        std::snprintf(status_buf, sizeof(status_buf), "%s (%s)",
                      entry.name.c_str(), format_size(entry.size).c_str());
      }
    }
  } else if (sel_count > 1) {
    uint64_t total_size = 0;
    for (int sel : app.cur_tab().multi_selected) {
      int real_idx =
          (sel >= 0 && sel < static_cast<int>(app.cur_tab().visible_entries.size()))
              ? app.cur_tab().visible_entries[sel]
              : -1;
      if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()))
        total_size += app.cur_tab().entries[real_idx].size;
    }
    std::snprintf(status_buf, sizeof(status_buf), "%zu items selected (%s)",
                  sel_count, format_size(total_size).c_str());
  }

  if (sel_count == 0) {
    if ((app.search_active || app.recursive_search_active || app.r_search_active || app.r_recursive_search_active) && (!app.search_query.empty() || !app.r_search_query.empty())) {
      std::snprintf(status_buf, sizeof(status_buf), "%zu results",
                    app.cur_tab().entries.size());
    } else {
      // Aggregates are cached on the tab (rebuilt when entries change);
      // scanning 900k entries here every frame used to cost ~25 ms/frame.
      std::snprintf(status_buf, sizeof(status_buf),
                    "%d items (%d files, %d dirs)", app.cur_tab().cached_total_items,
                    app.cur_tab().cached_total_files, app.cur_tab().cached_total_dirs);
    }
  }

  if (!app.operation_status.empty()) {
    // Show operation status centered between the left text and the right
    // cluster, elided so it can never collide with either.
    constexpr int kCtlWOp = 24 + 12 + 220 + 12 + 24;
    const double right_reserve = kCtlWOp + 24 + 80; // ctl + gap + free text
    cairo_text_extents_t te;
    cairo_text_extents(cr, status_buf, &te);
    double left_w = te.x_advance;
    double zone_l = pad + left_w + static_cast<int>(16.0 * zf);
    double zone_r = static_cast<double>(w) - pad - right_reserve;
    double op_budget = zone_r - zone_l;
    std::string shown_op = app.operation_status;
    cairo_text_extents(cr, shown_op.c_str(), &te);
    if (te.x_advance > op_budget && op_budget > 60) {
      while (!shown_op.empty()) {
        cairo_text_extents(cr, (shown_op + "...").c_str(), &te);
        if (te.x_advance <= op_budget) break;
        shown_op.pop_back();
      }
      shown_op += "...";
    }
    double sw = te.x_advance;
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
    cairo_move_to(cr, zone_l + (op_budget - sw) / 2, y + status_h / 2 + 4);
    cairo_show_text(cr, shown_op.c_str());
  }

  // ── Free-space readout (cached statvfs, 2 s) ──
  std::string free_str;
  {
    static std::string s_path;
    static uint64_t s_free = 0;
    static bool s_valid = false;
    static std::chrono::steady_clock::time_point s_at{};
    auto now = std::chrono::steady_clock::now();
    if (!s_valid || s_path != app.cur_tab().current_path ||
        now - s_at > std::chrono::milliseconds(2000)) {
      struct statvfs sv;
      s_valid = statvfs(app.cur_tab().current_path.c_str(), &sv) == 0;
      if (s_valid) s_free = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
      s_path = app.cur_tab().current_path;
      s_at = now;
    }
    if (s_valid) free_str = format_size(s_free) + " free";
    // Don't refresh while the user is on the zoom control — a changing
    // string would shift the slider/buttons under the cursor.
    bool over_zoom_ctl =
        app.status_zoom_minus[2] > 0 &&
        app.pointerY >= y && app.pointerY < y + status_h &&
        app.pointerX >= app.status_zoom_minus[0] - 8 &&
        app.pointerX < app.status_zoom_plus[0] + app.status_zoom_plus[2] + 8;
    if (over_zoom_ctl)
      s_at = now; // hold the cache while hovering
  }

  cairo_text_extents_t te;
  cairo_text_extents(cr, status_buf, &te);
  double status_w = te.x_advance;

  cairo_text_extents(cr, free_str.c_str(), &te);
  double free_w = te.width;

  // Zoom control footprint (fixed geometry): minus+gap+track+gap+plus
  constexpr int kCtlW = 24 + 12 + 220 + 12 + 24;
  // Right cluster reserved to the right of the status text: zoom ctl +
  // gap + free text (+ margin). Conservative lower bound keeps the centered
  // operation status honest before free_str is measured.
  const double right_cluster =
      free_w + 24.0 + static_cast<double>(kCtlW);

  // Elide the left status text so it can never run under the right cluster
  double status_budget = static_cast<double>(w) - 2 * pad - right_cluster -
                         static_cast<int>(16.0 * zf);
  std::string shown_status = status_buf;
  if (status_w > status_budget && status_budget > 40) {
    while (!shown_status.empty()) {
      cairo_text_extents(cr, (shown_status + "...").c_str(), &te);
      if (te.x_advance <= status_budget) break;
      shown_status.pop_back();
    }
    shown_status += "...";
  }
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  cairo_move_to(cr, pad, y + status_h / 2 + 4);
  cairo_show_text(cr, shown_status.c_str());

  if (!free_str.empty()) {
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 1.0);
    cairo_move_to(cr, static_cast<double>(w) - pad - free_w, y + status_h / 2 + 4);
    cairo_show_text(cr, free_str.c_str());
  }

  // ── Zoom slider (discrete levels) + −/+ buttons ──
  {
    // Fixed geometry (NOT zoom-scaled): stepping +/- must never move the
    // control out from under the cursor.
    constexpr int kTrackW = 220;
    constexpr int kRightGap = 24;
    int track_w = kTrackW;
    int cy = y + status_h / 2;
    int btn_w = 24, btn_h = 18;
    int right_edge = w - pad - free_w - kRightGap;
    int minus_x = right_edge - (btn_w + 12 + track_w + 12 + btn_w);
    int track_x = minus_x + btn_w + 12;
    int plus_x = track_x + track_w + 12;
    int btn_y = cy - btn_h / 2;
    if (minus_x > pad + status_w + static_cast<int>(16.0 * zf)) {
      auto dim = [&](bool hov) {
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, hov ? 1.0 : 0.65);
      };
      app.status_zoom_minus[0] = minus_x;
      app.status_zoom_minus[1] = btn_y;
      app.status_zoom_minus[2] = btn_w;
      app.status_zoom_minus[3] = btn_h;
      app.status_zoom_plus[0] = plus_x;
      app.status_zoom_plus[1] = btn_y;
      app.status_zoom_plus[2] = btn_w;
      app.status_zoom_plus[3] = btn_h;
      app.status_zoom_slider_x = track_x;
      app.status_zoom_slider_w = track_w;

      auto in_rect = [&](const int* r) {
        return app.pointerX >= r[0] && app.pointerX < r[0] + r[2] &&
               app.pointerY >= r[1] && app.pointerY < r[1] + r[3];
      };
      auto pill = [&](const int* r, bool hover) {
        cairo_set_source_rgba(cr, app.surface_r, app.surface_g,
                              app.surface_b, hover ? 0.95 : 0.55);
        draw_rounded_rect(cr, static_cast<double>(r[0]),
                          static_cast<double>(r[1]),
                          static_cast<double>(r[2]),
                          static_cast<double>(r[3]), 9);
        cairo_fill(cr);
        if (hover) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.55);
          cairo_set_line_width(cr, 1.2);
          draw_rounded_rect(cr, static_cast<double>(r[0]) + 0.5,
                            static_cast<double>(r[1]) + 0.5,
                            static_cast<double>(r[2]) - 1,
                            static_cast<double>(r[3]) - 1, 9);
          cairo_stroke(cr);
        }
      };
      // minus button
      bool m_hov = in_rect(app.status_zoom_minus);
      pill(app.status_zoom_minus, m_hov);
      dim(m_hov ? 1.0 : 0.7);
      cairo_set_line_width(cr, 1.6);
      double mcx = minus_x + btn_w / 2.0;
      cairo_move_to(cr, mcx - 5, cy + 0.5);
      cairo_line_to(cr, mcx + 5, cy + 0.5);
      cairo_stroke(cr);
      // plus button
      bool p_hov = in_rect(app.status_zoom_plus);
      pill(app.status_zoom_plus, p_hov);
      dim(p_hov ? 1.0 : 0.7);
      cairo_set_line_width(cr, 1.6);
      double pcx = plus_x + btn_w / 2.0;
      cairo_move_to(cr, pcx - 5, cy + 0.5);
      cairo_line_to(cr, pcx + 5, cy + 0.5);
      cairo_stroke(cr);
      cairo_move_to(cr, pcx, cy - 5 + 0.5);
      cairo_line_to(cr, pcx, cy + 5 + 0.5);
      cairo_stroke(cr);
      // track
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.45);
      cairo_rectangle(cr, track_x, cy - 2, track_w, 3);
      cairo_fill(cr);
      // handle at current level
      double t = static_cast<double>(zoom_level_for_pct(app.settings_zoom_pct)) /
                 (kZoomLevelCount - 1);
      double hx = track_x + t * (track_w - 10);
      bool hov = app.status_zoom_dragging ||
                 (app.pointerY >= cy - 12 && app.pointerY < cy + 12 &&
                  app.pointerX >= track_x && app.pointerX < track_x + track_w);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                            hov ? 1.0 : 0.9);
      cairo_arc(cr, hx + 5, cy, 5, 0, 2 * M_PI);
      cairo_fill(cr);
      if (hov) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.3);
        cairo_set_line_width(cr, 1.4);
        cairo_arc(cr, hx + 5, cy, 8, 0, 2 * M_PI);
        cairo_stroke(cr);
      }
    } else {
      app.status_zoom_slider_x = 0;
      app.status_zoom_slider_w = 0;
      app.status_zoom_minus[2] = 0;
      app.status_zoom_plus[2] = 0;
    }
  }
}

// ── directory picker bar ─────────────────────────────────────────

void draw_select_dir_bar(AppState& app, cairo_t* cr, int w, int h,
                         int bar_h) {
  double zf = app.zoom_pct / 100.0;
  int y = h - app.status_bar_height - bar_h;
  app.select_bar_y = y;
  double sa = app.statusbar_opacity_pct / 100.0;

  // Background
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, sa);
  cairo_rectangle(cr, 0, y, w, bar_h);
  cairo_fill(cr);

  // border-t
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  cairo_rectangle(cr, 0, y, w, 1);
  cairo_fill(cr);

  int pad = static_cast<int>(24.0 * zf);

  // "Select:" label + path/file
  std::string label;
  if (app.select_file_mode) {
    auto& tab = app.cur_tab();
    if (tab.selected_idx >= 0 && tab.selected_idx < static_cast<int>(tab.visible_entries.size())) {
      auto& fe = tab.entries[tab.visible_entries[tab.selected_idx]];
      label = "Select: " + fe.path;
    } else {
      label = "Select: (select a file)";
    }
  } else {
    label = "Select: " + app.cur_tab().current_path;
  }
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * zf);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
  cairo_move_to(cr, pad, y + bar_h / 2 + 4);
  cairo_show_text(cr, label.c_str());

  // ── Select button ──
  int btn_w = static_cast<int>(80.0 * zf);
  int btn_h = static_cast<int>(28.0 * zf);
  int btn_gap = static_cast<int>(8.0 * zf);
  int sel_x = w - pad - btn_w;
  int can_x = sel_x - btn_gap - btn_w;
  int btn_y = y + (bar_h - btn_h) / 2;

  app.select_btn_x = sel_x;
  app.select_btn_w = btn_w;
  app.cancel_btn_x = can_x;
  app.cancel_btn_w = btn_w;

  // Select button
  double r = 4.0 * zf;
  if (app.select_btn_hover) {
    cairo_set_source_rgba(cr, 0.3, 0.5, 1.0, 1.0);
  } else {
    cairo_set_source_rgba(cr, 0.2, 0.4, 0.9, 1.0);
  }
  draw_rounded_rect(cr, static_cast<double>(sel_x), static_cast<double>(btn_y),
                    static_cast<double>(btn_w), static_cast<double>(btn_h), r);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
  cairo_set_font_size(cr, 13.0 * zf);
  cairo_text_extents_t te;
  cairo_text_extents(cr, "Select", &te);
  cairo_move_to(cr, sel_x + (btn_w - te.width) / 2,
                btn_y + (btn_h - te.height) / 2 - te.y_bearing);
  cairo_show_text(cr, "Select");

  // Cancel button
  if (app.cancel_btn_hover) {
    cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 1.0);
  } else {
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 1.0);
  }
  draw_rounded_rect(cr, static_cast<double>(can_x), static_cast<double>(btn_y),
                    static_cast<double>(btn_w), static_cast<double>(btn_h), r);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 1.0);
  cairo_text_extents(cr, "Cancel", &te);
  cairo_move_to(cr, can_x + (btn_w - te.width) / 2,
                btn_y + (btn_h - te.height) / 2 - te.y_bearing);
  cairo_show_text(cr, "Cancel");
}

} // namespace eh::file_browser