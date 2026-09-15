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

// ── preview/thumbnail debug logger ─────────────────────────────────
static std::mutex g_preview_log_mtx;
void preview_log(const char* fmt, ...) {
  static FILE* f = nullptr;
  std::lock_guard<std::mutex> lock(g_preview_log_mtx);
  if (!f) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/horizon-files-previews.log";
    f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "preview log started\n");
    fflush(f);
  }
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm* t = localtime(&ts.tv_sec);
  fprintf(f, "%02d:%02d:%02d.%03ld ", t->tm_hour, t->tm_min, t->tm_sec, ts.tv_nsec / 1000000);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fprintf(f, "\n");
  fflush(f);
}

// ── drawing helpers ──────────────────────────────────────────────

void draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h,
                       double r) {
  cairo_new_path(cr);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_arc(cr, x + w - r, y + r, r, 3 * M_PI / 2, 2 * M_PI);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_close_path(cr);
}





void draw_scrollbar(AppState& app, cairo_t* cr, int x, int y, int h,
                    int content_h, int view_h, int scroll_px, double r,
                    double g, double b, bool computer_view) {
  if (content_h <= view_h) return;

  // Record the interactive region so input can hit-test/drag this thumb.
  app.scrollbar_rects.push_back({x, y, 6, h, content_h, view_h, computer_view});

  double thumb_h = static_cast<double>(view_h) * static_cast<double>(view_h) /
                   static_cast<double>(content_h);
  double max_scroll = static_cast<double>(content_h - view_h);
  double thumb_y =
      static_cast<double>(scroll_px) / max_scroll * (view_h - thumb_h);

  cairo_set_source_rgba(cr, r, g, b, 0.15);
  cairo_rectangle(cr, static_cast<double>(x), static_cast<double>(y), 6.0,
                  static_cast<double>(h));
  cairo_fill(cr);

  cairo_set_source_rgba(cr, r, g, b, 0.4);
  draw_rounded_rect(cr, static_cast<double>(x), y + thumb_y, 6.0,
                    std::max(thumb_h, 20.0), 3.0);
  cairo_fill(cr);
}







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
// Single source of truth for hit-testing. Mirrors the layout math in
// draw() (app.cpp) exactly, including search-banner / select-bar /
// info-panel / ops-panel insets and the per-pane top bar. The pane is
// derived FROM THE POINTER X, so results stay position-correct no matter
// which pane currently has focus.
// Tab whose entry list lives in the pane under px.
static Tab& pane_tab_at(AppState& app, int px) {
  if (app.split_view && pane_view_rect_at(app, px).pane == 1)
    return app.right_pane;
  return app.tabs[app.active_tab];
}

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

// ── context menu drawing ─────────────────────────────────────────

namespace {

constexpr int kMenuRowH = 34;
constexpr int kMenuSepH = 9;

int menu_items_height(const std::vector<AppState::ContextMenuItem>& items) {
  int h = 0;
  for (const auto& item : items) {
    h += (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty())
             ? kMenuSepH
             : kMenuRowH;
  }
  return h;
}

void clamp_popup_rect(const AppState& app, int& x, int& y, int w, int h) {
  if (x + w > app.width - 8) x = app.width - w - 8;
  if (y + h > app.height - 8) y = app.height - h - 8;
  if (x < 8) x = 8;
  if (y < 8) y = 8;
}

struct ContextMenuGeometry {
  int cm_x = 0, cm_y = 0, cm_w = 240, cm_h = 0;
};

ContextMenuGeometry context_menu_geometry(AppState& app) {
  ContextMenuGeometry g;
  g.cm_x = app.context_menu_x;
  g.cm_y = app.context_menu_y;
  g.cm_h = menu_items_height(app.context_menu_items);
  clamp_popup_rect(app, g.cm_x, g.cm_y, g.cm_w, g.cm_h);
  return g;
}

struct SubmenuGeometry {
  int sub_x = 0, sub_y = 0, sm_w = 200, sm_h = 0;
};

SubmenuGeometry submenu_geometry(AppState& app, int hover) {
  SubmenuGeometry g;
  const auto& items = app.context_menu_items;
  const ContextMenuGeometry m = context_menu_geometry(app);
  g.sm_h = menu_items_height(items[static_cast<size_t>(hover)].sub_items);
  g.sub_x = m.cm_x + m.cm_w + 3;
  if (g.sub_x + g.sm_w > app.width - 8) g.sub_x = m.cm_x - g.sm_w - 3;
  g.sub_y = m.cm_y;
  for (int j = 0; j < hover; ++j) {
    g.sub_y += (items[static_cast<size_t>(j)].action == AppState::ContextMenuAction::Separator &&
                items[static_cast<size_t>(j)].sub_items.empty())
                   ? kMenuSepH
                   : kMenuRowH;
  }
  clamp_popup_rect(app, g.sub_x, g.sub_y, g.sm_w, g.sm_h);
  return g;
}

} // namespace

static void draw_submenu_popup(AppState& app, cairo_t* cr, const std::vector<AppState::ContextMenuItem>& items,
                                int px, int py, int* out_w, int* out_h) {
  int sm_w = 200;
  int sm_h = 0;
  for (const auto& item : items) {
    sm_h += (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) ? 9 : 34;
  }

  int sm_x = px;
  int sm_y = py;
  if (sm_y + sm_h > app.height - 8) sm_y = app.height - sm_h - 8;
  if (sm_y < 8) sm_y = 8;

  // Drop shadow (3 layers)
  for (int s = 3; s >= 0; --s) {
    double a = 0.12 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, sm_x + s * 2.5, sm_y + s * 3, sm_w, sm_h, 10);
    cairo_fill(cr);
  }

  // Card background
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
  draw_rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 10);
  cairo_fill_preserve(cr);

  // Border
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.30);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  int ry = sm_y;
  for (size_t i = 0; i < items.size(); ++i) {
    const auto& item = items[i];
    if (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) {
      ry += 4;
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.18);
      cairo_set_line_width(cr, 1);
      cairo_move_to(cr, sm_x + 14, ry + 0.5);
      cairo_line_to(cr, sm_x + sm_w - 14, ry + 0.5);
      cairo_stroke(cr);
      ry += 5;
      continue;
    }

    int row_h = 34;
    bool hovered = (static_cast<int>(i) == app.context_menu_sub_hover);

    // Hover highlight
    if (hovered) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.14);
      draw_rounded_rect(cr, sm_x + 5, ry + 2, sm_w - 10, row_h - 4, 6);
      cairo_fill(cr);
    }

    // Label
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_text_extents_t te;
    cairo_text_extents(cr, item.label.c_str(), &te);
    double tx = sm_x + 16;
    double ty = ry + (row_h - te.height) / 2.0 - te.y_bearing;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, item.label.c_str());
    ry += row_h;
  }

  if (out_w) *out_w = sm_w;
  if (out_h) *out_h = sm_h;
}

void draw_context_menu(AppState& app, cairo_t* cr) {
  const ContextMenuGeometry g = context_menu_geometry(app);
  int cm_x = g.cm_x;
  int cm_y = g.cm_y;
  int cm_w = g.cm_w;
  int cm_h = g.cm_h;

  // Drop shadow (3 layers, heavier)
  for (int s = 3; s >= 0; --s) {
    double a = 0.14 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, cm_x + s * 2.5, cm_y + s * 3, cm_w, cm_h, 10);
    cairo_fill(cr);
  }

  // Card background
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
  draw_rounded_rect(cr, cm_x, cm_y, cm_w, cm_h, 10);
  cairo_fill_preserve(cr);

  // Border
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.30);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  int ry = cm_y;
  for (int i = 0; i < static_cast<int>(app.context_menu_items.size()); ++i) {
    const auto& item = app.context_menu_items[i];
    if (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) {
      ry += 4;
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.18);
      cairo_set_line_width(cr, 1);
      cairo_move_to(cr, cm_x + 14, ry + 0.5);
      cairo_line_to(cr, cm_x + cm_w - 14, ry + 0.5);
      cairo_stroke(cr);
      ry += 5;
      continue;
    }

    int row_h = 34;
    bool has_sub = !item.sub_items.empty();
    bool hovered = (i == app.context_menu_hover);

    // Hover highlight
    if (hovered) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.14);
      draw_rounded_rect(cr, cm_x + 5, ry + 2, cm_w - 10, row_h - 4, 6);
      cairo_fill(cr);
    }

    // Label text with proper vertical centering
    bool destructive = (item.action == AppState::ContextMenuAction::MoveToTrash ||
                        item.action == AppState::ContextMenuAction::PermanentDelete);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            has_sub ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    if (destructive) {
      cairo_set_source_rgba(cr, 0.95, 0.30, 0.30, 0.90);
    } else {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    }
    cairo_text_extents_t te;
    cairo_text_extents(cr, item.label.c_str(), &te);
    double tx = cm_x + 16;
    double ty = ry + (row_h - te.height) / 2.0 - te.y_bearing;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, item.label.c_str());

    // Submenu arrow chevron
    if (has_sub) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.45);
      cairo_set_line_width(cr, 1.5);
      int ax = cm_x + cm_w - 18;
      int ay = ry + row_h / 2;
      cairo_move_to(cr, ax - 1, ay - 4);
      cairo_line_to(cr, ax + 4, ay);
      cairo_line_to(cr, ax - 1, ay + 4);
      cairo_stroke(cr);
    }

    ry += row_h;
  }

  // Draw open submenu
  if (app.context_menu_hover >= 0 &&
      static_cast<size_t>(app.context_menu_hover) < app.context_menu_items.size()) {
    const auto& item = app.context_menu_items[app.context_menu_hover];
    if (!item.sub_items.empty()) {
      const SubmenuGeometry sg = submenu_geometry(app, app.context_menu_hover);
      draw_submenu_popup(app, cr, item.sub_items, sg.sub_x, sg.sub_y, nullptr, nullptr);
    }
  }
}

// ── hit testing ──────────────────────────────────────────────────

int hit_test_list(AppState& app, int x, int y) {
  PaneViewRect r = pane_view_rect_at(app, x);
  Tab& tab = pane_tab_at(app, x);

  if (x < r.x || x >= r.x + r.w) return -1;
  if (y < r.y || y >= r.y + r.h) return -1;

  int col_header_h = app.entry_height;
  int scroll = tab.scroll_px;

  int rel_y = y - r.y - col_header_h + scroll;
  if (rel_y < 0) return -1;

  if (!tab.group_by_type) {
    int idx = rel_y / app.entry_height;
    if (idx < 0 || idx >= static_cast<int>(tab.visible_entries.size()))
      return -1;
    return idx;
  }

  int hdr_h = static_cast<int>(app.entry_height * 0.55);
  int acc = 0;
  int prev_type = -1;
  for (int vi = 0; vi < static_cast<int>(tab.visible_entries.size()); ++vi) {
    int ri = tab.visible_entries[vi];
    if (ri >= 0 && ri < static_cast<int>(tab.entries.size())) {
      int t = static_cast<int>(tab.entries[ri].type);
      if (t != prev_type) { acc += hdr_h; prev_type = t; }
    }
    if (rel_y >= acc && rel_y < acc + app.entry_height) return vi;
    acc += app.entry_height;
  }
  return -1;
}

int hit_test_grid(AppState& app, int x, int y) {
  PaneViewRect r = pane_view_rect_at(app, x);
  Tab& tab = pane_tab_at(app, x);

  if (x < r.x || x >= r.x + r.w) return -1;
  if (y < r.y || y >= r.y + r.h) return -1;

  // Recompute layout from THIS pane's width — same single source of truth
  // that draw_grid_view uses (layout.cpp). No more stale app.grid_* globals
  // in split view, and the bucket_down snap is applied identically.
  GridLayout gl = compute_grid_layout(r.w, app.zoom_pct / 100.0);
  int cols = gl.cols;
  int cell_size = gl.cell_w;
  int icon_size = gl.icon_size;
  int label_h = gl.label_h;
  int text_gap = gl.text_gap;
  int item_h = gl.item_h;
  int row_h = gl.row_h;
  int row_gap = gl.row_gap;
  int col_gap = gl.col_gap;
  int grid_w = gl.grid_w;
  int grid_offset_x = gl.grid_offset_x;
  if (row_h <= 0) return -1;

  int rel_x = x - r.x - grid_offset_x;
  int rel_y = y - r.y - row_gap + tab.scroll_px;

  int col = (rel_x + col_gap / 2) / (cell_size + col_gap);
  int row = (rel_y + row_gap / 2) / row_h;

  int idx = row * cols + col;
  if (idx < 0 || idx >= static_cast<int>(tab.visible_entries.size()))
    return -1;

  int cx = col * (cell_size + col_gap);
  int cy = row * row_h;
  if (rel_x < cx || rel_x > cx + cell_size) return -1;
  if (rel_y < cy || rel_y > cy + item_h) return -1;

  return idx;
}

int hit_test_context_menu(AppState& app, int x, int y) {
  if (!app.context_menu_open) return -1;
  const ContextMenuGeometry g = context_menu_geometry(app);
  int cm_x = g.cm_x;
  int cm_y = g.cm_y;
  int cm_w = g.cm_w;
  int cm_h = g.cm_h;

  // Check submenu first if hovered item has one
  if (app.context_menu_hover >= 0 &&
      static_cast<size_t>(app.context_menu_hover) < app.context_menu_items.size() &&
      !app.context_menu_items[app.context_menu_hover].sub_items.empty()) {
    const SubmenuGeometry sg = submenu_geometry(app, app.context_menu_hover);
    const auto& subs = app.context_menu_items[app.context_menu_hover].sub_items;
    if (x >= sg.sub_x && x < sg.sub_x + sg.sm_w && y >= sg.sub_y && y < sg.sub_y + sg.sm_h) {
      // Hit on submenu - return index encoded as negative offset from -10
      int rel_y = y - sg.sub_y;
      for (size_t i = 0; i < subs.size(); ++i) {
        int h = (subs[i].action == AppState::ContextMenuAction::Separator && subs[i].sub_items.empty()) ? 9 : 34;
        if (rel_y < h) {
          if (subs[i].action == AppState::ContextMenuAction::Separator) return -1;
          return -10 - static_cast<int>(i);
        }
        rel_y -= h;
      }
      return -1;
    }
  }

  // Check main menu
  if (x < cm_x || x >= cm_x + cm_w || y < cm_y || y >= cm_y + cm_h)
    return -1;
  int rel_y = y - cm_y;
  for (size_t i = 0; i < app.context_menu_items.size(); ++i) {
    int h = (app.context_menu_items[i].action == AppState::ContextMenuAction::Separator &&
             app.context_menu_items[i].sub_items.empty()) ? 9 : 34;
    if (rel_y < h) return static_cast<int>(i);
    rel_y -= h;
  }
  return -1;
}

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

  // Drop shadow (3 layers, lighter than the context menu)
  for (int s = 3; s >= 0; --s) {
    double a = 0.12 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, cx + s * 2.5, cy + s * 3, w, kChooserH, 10);
    cairo_fill(cr);
  }

  // Card background
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
  draw_rounded_rect(cr, cx, cy, w, kChooserH, 10);
  cairo_fill_preserve(cr);

  // Border
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.30);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

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
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.18);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, cx + 14, ry + 0.5);
  cairo_line_to(cr, cx + w - 14, ry + 0.5);
  cairo_stroke(cr);
  ry += 5;

  // Action rows: 0 = copy, 1 = move
  static const char* kDropActions[2] = {"Copy here", "Move here"};
  for (int i = 0; i < 2; ++i) {
    bool hovered = (i == app.drop_chooser_hover);

    if (hovered) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.14);
      draw_rounded_rect(cr, cx + 5, ry + 2, w - 10, kChooserRowH - 4, 6);
      cairo_fill(cr);
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
  if (!app.drop_chooser_open) return -1;
  const DropChooserGeometry g = drop_chooser_geometry(app);
  if (x < g.x || x >= g.x + g.w || y < g.y || y >= g.y + g.h)
    return -1;
  int rel_y = y - g.y;
  if (rel_y < kChooserHeaderH + kChooserSepH) return -1;
  int row = (rel_y - kChooserHeaderH - kChooserSepH) / kChooserRowH;
  if (row < 0 || row > 1) return -1;
  return row;
}

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



// ── Info panel (F11) ────────────────────────────────────────────

void draw_info_panel(AppState& app, cairo_t* cr) {
  if (!app.info_panel_open) return;

  double zf = app.zoom_pct / 100.0;
  int pw = app.info_panel_width;
  int px = app.width - pw;
  int top_h = app.top_bar_height + app.tab_bar_height;
  int ph = app.height - top_h - app.status_bar_height;
  int py = top_h;

  // Background (same tinted surface as sidebar)
  cairo_set_source_rgba(cr, app.surface_r * 2, app.surface_g * 2, app.surface_b * 2, 0.95);
  cairo_rectangle(cr, px, py, pw, ph);
  cairo_fill(cr);

  // Left separator
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_rectangle(cr, px, py, 1, ph);
  cairo_fill(cr);

  // ── Tab bar ──
  static const char* kTabNames[] = {"Preview", "Properties", "Terminal"};
  int tab_h = static_cast<int>(38 * zf);
  int tab_w = pw / 3;

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12 * zf);

  for (int i = 0; i < 3; ++i) {
    int tx = px + i * tab_w;
    app.info_panel_hit_tabs[i][0] = static_cast<double>(tx);
    app.info_panel_hit_tabs[i][1] = static_cast<double>(py);
    app.info_panel_hit_tabs[i][2] = static_cast<double>(tab_w);
    app.info_panel_hit_tabs[i][3] = static_cast<double>(tab_h);

    if (i == app.info_panel_tab) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
      cairo_rectangle(cr, static_cast<double>(tx), static_cast<double>(py),
                      static_cast<double>(tab_w), static_cast<double>(tab_h));
      cairo_fill(cr);
    }

    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                          i == app.info_panel_tab ? 0.95 : 0.55);
    cairo_text_extents_t te;
    cairo_text_extents(cr, kTabNames[i], &te);
    cairo_move_to(cr, tx + (tab_w - te.x_advance) / 2.0,
                  py + tab_h / 2.0 + te.height * 0.35);
    cairo_show_text(cr, kTabNames[i]);

    if (i == app.info_panel_tab) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
      cairo_rectangle(cr, tx + 6.0, py + tab_h - 2.5, tab_w - 12.0, 2.5);
      cairo_fill(cr);
    }
  }

  // Tab underline
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.15);
  cairo_rectangle(cr, static_cast<double>(px), static_cast<double>(py + tab_h),
                  static_cast<double>(pw), 1);
  cairo_fill(cr);

  int content_y = py + tab_h + 1;
  int content_h = ph - tab_h - 1;

  // ── Preview tab ──
  if (app.info_panel_tab == 0) {
    if (app.info_panel_path.empty() || app.info_panel_is_dir) {
      cairo_set_font_size(cr, 12 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 0.55);
      const char* msg = app.info_panel_path.empty() ? "No file selected"
                       : app.info_panel_is_dir ? "(folder)"
                       : "";
      if (*msg) {
        cairo_text_extents_t te;
        cairo_text_extents(cr, msg, &te);
        cairo_move_to(cr, px + (pw - te.x_advance) / 2.0,
                      content_y + content_h / 2.0);
        cairo_show_text(cr, msg);
      }
    } else {
      int thumb_px = pw - 24;
      cairo_surface_t* thumb = get_thumbnail(app, app.info_panel_path, thumb_px);
      if (thumb) {
        int tw = cairo_image_surface_get_width(thumb);
        int th = cairo_image_surface_get_height(thumb);
        if (tw > 0 && th > 0) {
          int avail_h = content_h - 96;
          double s = std::min(1.0, std::min(static_cast<double>(thumb_px) / tw,
                                            static_cast<double>(avail_h) / th));
          int dw = static_cast<int>(tw * s);
          int dh = static_cast<int>(th * s);
          int dx = px + (pw - dw) / 2;
          int dy = content_y + (avail_h - dh) / 2;
          cairo_save(cr);
          cairo_rectangle(cr, static_cast<double>(dx), static_cast<double>(dy),
                          static_cast<double>(dw), static_cast<double>(dh));
          cairo_clip(cr);
          cairo_set_source_surface(cr, thumb, static_cast<double>(dx),
                                   static_cast<double>(dy));
          cairo_paint(cr);
          cairo_restore(cr);
        }
      }
      // File name below preview
      std::string name = app.info_panel_name;
      if (name.size() > 24) {
        auto dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0) {
          std::string ext = name.substr(dot);
          name = name.substr(0, 21 - ext.size()) + "..." + ext;
        } else {
          name = name.substr(0, 21) + "...";
        }
      }
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.85);
      cairo_set_font_size(cr, 11 * zf);
      cairo_text_extents_t te;
      cairo_text_extents(cr, name.c_str(), &te);
      cairo_move_to(cr, px + (pw - te.x_advance) / 2.0,
                    py + ph - 14);
      cairo_show_text(cr, name.c_str());

      // ── Metadata rows beneath the preview ──
      auto meta_font = [&](double px_size) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, px_size * zf);
      };
      // Line 1: type
      std::string meta_type = app.info_panel_mime_type;
      if (meta_type.size() > 30) meta_type = meta_type.substr(0, 29) + "\u2026";
      if (!meta_type.empty()) {
        meta_font(10);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, 0.75);
        cairo_text_extents(cr, meta_type.c_str(), &te);
        cairo_move_to(cr, px + (pw - te.x_advance) / 2.0, py + ph - 50);
        cairo_show_text(cr, meta_type.c_str());
      }
      // Line 2: size · modified
      auto fmt_meta_size = [](uint64_t bytes) {
        char buf[32];
        double v = static_cast<double>(bytes);
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int ui = 0;
        while (v >= 1024.0 && ui < 4) { v /= 1024.0; ++ui; }
        if (ui == 0) snprintf(buf, sizeof(buf), "%llu B",
                              static_cast<unsigned long long>(bytes));
        else snprintf(buf, sizeof(buf), "%.1f %s", v, units[ui]);
        return std::string(buf);
      };
      char meta_time[32];
      {
        time_t mt = static_cast<time_t>(app.info_panel_modified_sec);
        struct tm* tm_local = localtime(&mt);
        strftime(meta_time, sizeof(meta_time), "%Y-%m-%d %H:%M", tm_local);
      }
      std::string meta_line =
          fmt_meta_size(app.info_panel_size) + " \u00b7 " + meta_time;
      meta_font(10);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 0.75);
      cairo_text_extents(cr, meta_line.c_str(), &te);
      cairo_move_to(cr, px + (pw - te.x_advance) / 2.0, py + ph - 33);
      cairo_show_text(cr, meta_line.c_str());
    }
  }

  // ── Properties tab ──
  else if (app.info_panel_tab == 1) {
    auto fmt_size = [](uint64_t bytes) -> std::string {
      if (bytes < 1024ULL) return std::to_string(bytes) + " B";
      if (bytes < 1024ULL * 1024) return std::to_string(bytes / 1024) + " KB";
      if (bytes < 1024ULL * 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
      return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
    };
    auto fmt_date = [](int64_t sec) -> std::string {
      char buf[32];
      struct tm tm;
      localtime_r(&sec, &tm);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
      return buf;
    };

    int ly = content_y + 16;
    int margin = 10;
    int col1_x = px + margin;
    int col2_x = px + pw / 2 + 4;

    auto draw_row = [&](const char* label, const std::string& value) {
      cairo_set_font_size(cr, 11 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 0.7);
      cairo_move_to(cr, static_cast<double>(col1_x), static_cast<double>(ly));
      cairo_show_text(cr, label);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
      cairo_move_to(cr, static_cast<double>(col2_x), static_cast<double>(ly));
      cairo_show_text(cr, value.c_str());
      ly += 22;
    };

    draw_row("Name", app.info_panel_name);
    draw_row("Size", fmt_size(app.info_panel_size));

    // File type
    if (app.cur_tab().selected_idx >= 0) {
      int si = app.cur_tab().selected_idx;
      int ri = app.cur_tab().visible_entries[si];
      if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size())) {
        static const char* kTypeNames[] = {"Folder", "Image", "Audio", "Video", "Text",
                                           "Markdown", "Code", "Document", "Font",
                                           "Archive", "Executable", "Web", "File"};
        int ti = static_cast<int>(app.cur_tab().entries[ri].type);
        if (ti >= 0 && ti < 13)
          draw_row("Type", kTypeNames[ti]);
      }
    }

    draw_row("Modified", fmt_date(app.info_panel_modified_sec));
    draw_row("Owner", app.info_panel_owner);
    draw_row("Group", app.info_panel_group);
    if (!app.info_panel_mime_type.empty())
      draw_row("MIME", app.info_panel_mime_type);

    // Permissions string
    {
      mode_t m = app.info_panel_mode;
      char perm[11] = {};
      perm[0] = S_ISDIR(m) ? 'd' : '-';
      perm[1] = (m & S_IRUSR) ? 'r' : '-';
      perm[2] = (m & S_IWUSR) ? 'w' : '-';
      perm[3] = (m & S_IXUSR) ? 'x' : '-';
      perm[4] = (m & S_IRGRP) ? 'r' : '-';
      perm[5] = (m & S_IWGRP) ? 'w' : '-';
      perm[6] = (m & S_IXGRP) ? 'x' : '-';
      perm[7] = (m & S_IROTH) ? 'r' : '-';
      perm[8] = (m & S_IWOTH) ? 'w' : '-';
      perm[9] = (m & S_IXOTH) ? 'x' : '-';
      draw_row("Permissions", perm);
    }
  }

  // ── Terminal tab (stub) ──
  else if (app.info_panel_tab == 2) {
    cairo_set_font_size(cr, 12 * zf);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 0.55);
    const char* msg = "Terminal (not implemented)";
    cairo_text_extents_t te;
    cairo_text_extents(cr, msg, &te);
    cairo_move_to(cr, px + (pw - te.x_advance) / 2.0,
                  content_y + content_h / 2.0);
    cairo_show_text(cr, msg);
  }
}

// ── Operations panel (right sidebar) ────────────────────────────

void draw_operations_panel(AppState& app, cairo_t* cr) {
  if (app.ops_panel_slide < 0.01) return;
  if (!app.op_progress) return;

  double zf = app.zoom_pct / 100.0;
  int pw = app.ops_panel_width;
  int slide_w = static_cast<int>(pw * app.ops_panel_slide);
  int px = app.width - slide_w;
  int top_h = app.top_bar_height + app.tab_bar_height;
  int ph = app.height - top_h - app.status_bar_height;
  int py = top_h;

  // Background
  cairo_set_source_rgba(cr, app.surface_r * 2, app.surface_g * 2, app.surface_b * 2, 0.95);
  cairo_rectangle(cr, px, py, slide_w, ph);
  cairo_fill(cr);

  // Left separator
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_rectangle(cr, px, py, 1, ph);
  cairo_fill(cr);

  if (app.ops_panel_slide < 0.5) return;

  auto& p = *app.op_progress;
  int total = p.total_files;
  int done = p.copied_files;
  bool counting = total == 0 && p.active;

  int content_x = px + 16;
  int content_w = slide_w - 32;
  int y = py + 24;

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

  // ── Header ──
  {
    const char* hdr = "FILE OPERATIONS";
    if (p.type == OperationType::Extract) hdr = "EXTRACTION";
    else if (p.type == OperationType::Move) hdr = "MOVE";
    else hdr = "COPY";
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.6);
    cairo_set_font_size(cr, 11.0 * zf);
    cairo_move_to(cr, content_x, y + 12 * zf);
    cairo_show_text(cr, hdr);
    y += 28 * static_cast<int>(zf);
  }

  // ── Current file ──
  {
    std::string label;
    if (p.type == OperationType::Extract)
      label = "Extracting";
    else if (p.type == OperationType::Move)
      label = "Moving";
    else
      label = "Copying";
    if (!p.current_file.empty()) {
      label += " ";
      std::string fname = p.current_file;
      int max_chars = static_cast<int>(content_w / (7.0 * zf));
      if (max_chars < 10) max_chars = 10;
      if (static_cast<int>(fname.size()) > max_chars) {
        auto dot = fname.rfind('.');
        if (dot != std::string::npos && dot > 0) {
          std::string ext = fname.substr(dot);
          int keep = max_chars - 3 - static_cast<int>(ext.size());
          if (keep > 0)
            fname = fname.substr(0, static_cast<size_t>(keep)) + "..." + ext;
          else
            fname = fname.substr(0, static_cast<size_t>(max_chars - 3)) + "...";
        } else {
          fname = fname.substr(0, static_cast<size_t>(max_chars - 3)) + "...";
        }
      }
      label += fname;
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 13.0 * zf);
    cairo_move_to(cr, content_x, y + 14 * zf);
    cairo_show_text(cr, label.c_str());
    y += 30 * static_cast<int>(zf);
  }

  // ── Progress bar ──
  {
    int bar_h = static_cast<int>(10 * zf);
    double bar_pct = counting ? 0.0 : (total > 0 ? static_cast<double>(done) / total : 0.0);
    int fill_w = static_cast<int>(content_w * bar_pct);

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
    draw_rounded_rect(cr, content_x, y, content_w, bar_h, static_cast<int>(4 * zf));
    cairo_fill(cr);

    if (fill_w > 0) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.8);
      draw_rounded_rect(cr, content_x, y, fill_w, bar_h, static_cast<int>(4 * zf));
      cairo_fill(cr);
    }
    y += bar_h + 12 * static_cast<int>(zf);
  }

  // ── File count ──
  {
    char buf[64];
    if (counting) {
      std::snprintf(buf, sizeof(buf), "Counting files\u2026");
    } else {
      int pct = total > 0 ? static_cast<int>(100.0 * done / total) : 0;
      std::snprintf(buf, sizeof(buf), "%d%%  (%d / %d files)", pct, done, total);
    }
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
    cairo_set_font_size(cr, 11.0 * zf);
    cairo_move_to(cr, content_x, y + 12 * zf);
    cairo_show_text(cr, buf);
    y += 22 * static_cast<int>(zf);
  }

  // ── Speed ──
  if (p.total_bytes.load() > 0) {
    double speed = p.speed_mbps();
    char buf[64];
    if (speed < 0.01)
      std::snprintf(buf, sizeof(buf), "Calculating speed\u2026");
    else
      std::snprintf(buf, sizeof(buf), "%.1f MB/s", speed);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
    cairo_set_font_size(cr, 11.0 * zf);
    cairo_move_to(cr, content_x, y + 12 * zf);
    cairo_show_text(cr, buf);
    y += 22 * static_cast<int>(zf);

    // ── Time remaining ──
    if (p.active) {
      double eta = p.eta_seconds();
      if (eta >= 0) {
        int mins = static_cast<int>(eta) / 60;
        int secs = static_cast<int>(eta) % 60;
        char eta_buf[64];
        if (mins > 0)
          std::snprintf(eta_buf, sizeof(eta_buf), "~%dm %ds remaining", mins, secs);
        else
          std::snprintf(eta_buf, sizeof(eta_buf), "~%ds remaining", secs);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, 0.7);
        cairo_set_font_size(cr, 10.0 * zf);
        cairo_move_to(cr, content_x, y + 12 * zf);
        cairo_show_text(cr, eta_buf);
        y += 20 * static_cast<int>(zf);
      }
    }
  }

  // ── Cancel button ──
  {
    int btn_size = static_cast<int>(18 * zf);
    int btn_x = px + slide_w - btn_size - 12;
    int btn_y = py + 10;
    app.ops_cancel_x = btn_x;
    app.ops_cancel_y = btn_y;
    app.ops_cancel_w = btn_size;
    app.ops_cancel_h = btn_size;

    int pad = static_cast<int>(4 * zf);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.5);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, btn_x + pad, btn_y + pad);
    cairo_line_to(cr, btn_x + btn_size - pad, btn_y + btn_size - pad);
    cairo_move_to(cr, btn_x + btn_size - pad, btn_y + pad);
    cairo_line_to(cr, btn_x + pad, btn_y + btn_size - pad);
    cairo_stroke(cr);
  }
}



// ── Icon prewarm ─────────────────────────────────────────────────
//
// draw_file_icon_cairo resolves icons through the ASYNC cache path, so on a
// freshly-scanned folder the first frames paint letter placeholders while
// the worker rasterizes. Prewarming synchronously right after apply_scan_result
// (same candidate order + size buckets as the painters) makes the very first
// paint final. Unique-name dedup keeps the stall bounded to a few ms even
// for large folders; the budget is a hard cap against pathological themes.
void prewarm_tab_icons(AppState& app) {
  auto& tab = app.cur_tab();
  if (tab.visible_entries.empty()) return;
  const double zf = app.zoom_pct / 100.0;
  // Match painter request sizes so cache buckets line up exactly: grid draws
  // at min(cell_w - 16*zf, 72*zf) snapped down to a cache bucket; non-grid
  // rows draw ~24*zf icons. Prewarming must request the SAME bucket or every
  // grid icon misses on the first paint and scale-blits until the worker
  // fills it — we want the exact-fit batch path from frame 1.
  int px = eh::icons::IconCache::bucket_down(static_cast<int>(24.0 * zf));
  if (tab.view_mode == ViewMode::Grid)
    px = eh::icons::IconCache::bucket_down(static_cast<int>(72.0 * zf));

  std::unordered_set<std::string> seen;
  seen.reserve(tab.visible_entries.size() * 2);
  int budget = 400;
  for (int vi : tab.visible_entries) {
    if (vi < 0 || vi >= static_cast<int>(tab.entries.size())) continue;
    const auto& e = tab.entries[vi];
    if (!e.icon_name.empty() && seen.insert(e.icon_name).second) {
      app.icons.tray_icon_sync(e.icon_name, px);
      if (--budget <= 0) return;
    }
    const char* type_name = icon_name_for_file_type(e.type, &e.path);
    if (type_name && *type_name && seen.insert(type_name).second) {
      app.icons.tray_icon_sync(type_name, px);
      if (--budget <= 0) return;
    }
  }
}

// ── Hit-test: tree view ──────────────────────────────────────────
int hit_test_tree(AppState& app, int x, int y, bool for_click) {
  PaneViewRect r = pane_view_rect_at(app, x);
  Tab& tab = pane_tab_at(app, x);
  if (tab.tree_entries.empty() || tab.tree_entries_dirty) build_tree_entries(app);
  if (tab.tree_entries.empty()) return -1;

  double zf = app.zoom_pct / 100.0;
  int entry_h = static_cast<int>(28.0 * zf);
  int indent_step = static_cast<int>(24.0 * zf);
  int arrow_w = static_cast<int>(16.0 * zf);

  int content_x = r.x;
  int content_y = r.y;
  if (x < content_x || x >= content_x + r.w || y < content_y ||
      y >= content_y + r.h)
    return -1;

  int rel_y = y - content_y + tab.scroll_px;
  int idx = rel_y / entry_h;
  if (idx < 0 || idx >= static_cast<int>(tab.tree_entries.size())) return -1;

  // Check if click is on expand/collapse arrow
  auto& te = tab.tree_entries[idx];
  int indent = te.depth * indent_step;
  int arrow_x_min = content_x + indent + 4;
  int arrow_x_max = arrow_x_min + arrow_w;
  int arrow_y = content_y + idx * entry_h - tab.scroll_px + (entry_h - arrow_w) / 2;
  if (te.is_dir && x >= arrow_x_min && x < arrow_x_max &&
      y >= arrow_y && y < arrow_y + arrow_w) {
    if (for_click) {
      if (tab.tree_expanded.count(te.path))
        tab.tree_expanded.erase(te.path);
      else
        tab.tree_expanded.insert(te.path);
      tab.hover_idx = idx;
    }
    return -2; // arrow hit
  }

  return idx;
}

// ── Hit-test: compact view ───────────────────────────────────────
int hit_test_compact(AppState& app, int x, int y) {
  PaneViewRect r = pane_view_rect_at(app, x);
  Tab& tab = pane_tab_at(app, x);
  if (tab.visible_entries.empty()) return -1;
  if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return -1;

  double zf = app.zoom_pct / 100.0;
  int entry_h = static_cast<int>(24.0 * zf);

  int rel_y = y - r.y + tab.scroll_px;
  int idx = rel_y / entry_h;
  if (idx < 0 || idx >= static_cast<int>(tab.visible_entries.size())) return -1;
  return idx;
}

} // namespace eh::file_browser
