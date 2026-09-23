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

// ── Open With dialog ──────────────────────────────────────────────

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
    else if (p.type == OperationType::Compress) hdr = "COMPRESSION";
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
    else if (p.type == OperationType::Compress)
      label = "Compressing";
    else
      label = "Copying";
    std::string cur = p.get_current_file();
    if (!cur.empty()) {
      label += " ";
      std::string fname = std::move(cur);
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
  // Workers maintain p.progress directly (copy/move/extract mirror done/total;
  // compress blends file completions, 7z overall % and output growth).
  // Compress pre-scan (no totals yet) keeps an animated stripe instead.
  {
    int bar_h = static_cast<int>(10 * zf);
    double live = p.progress.load();
    if (live < 0) live = 0;
    if (live > 1) live = 1;
    bool indeterminate =
        (p.type == OperationType::Compress && p.active.load() && counting);

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
    draw_rounded_rect(cr, content_x, y, content_w, bar_h, static_cast<int>(4 * zf));
    cairo_fill(cr);

    if (indeterminate) {
      double elapsed = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - p.start_time).count();
      int stripe_w = std::max(20, content_w / 3);
      int travel = content_w + stripe_w;
      int off = static_cast<int>(std::fmod(elapsed * content_w * 0.7,
                                           static_cast<double>(travel))) - stripe_w;
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.8);
      cairo_save(cr);
      draw_rounded_rect(cr, content_x, y, content_w, bar_h, static_cast<int>(4 * zf));
      cairo_clip(cr);
      cairo_rectangle(cr, content_x + off, y, stripe_w, bar_h);
      cairo_fill(cr);
      cairo_restore(cr);
    } else {
      int fill_w = counting ? 0 : static_cast<int>(content_w * live);
      if (fill_w > 0) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.8);
        draw_rounded_rect(cr, content_x, y, fill_w, bar_h, static_cast<int>(4 * zf));
        cairo_fill(cr);
      }
    }
    y += bar_h + 12 * static_cast<int>(zf);
  }

  // ── File count ──
  {
    char buf[64];
    double live = p.progress.load();
    if (live < 0) live = 0;
    if (live > 1) live = 1;
    if (p.type == OperationType::Compress && p.active.load() && counting) {
      std::snprintf(buf, sizeof(buf), "Preparing\u2026");
    } else if (counting) {
      std::snprintf(buf, sizeof(buf), "Counting files\u2026");
    } else {
      int pct = static_cast<int>(100.0 * live);
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
