// window.cpp — disk usage analyzer window: lifecycle, paint, input.
//
// Geometry is fixed at open (1120x760, min==max like settings) in the
// file manager's dark-card design language:
//   title bar | toolbar (Rescan/Stop, breadcrumb, Zoom/Open/Delete)
//   drives | directory list | extension list
//   treemap | status bar

#include "app/file_browser/features/diskusage/diskusage.hpp"

#include "../../app.hpp"

#include "base/thread/thread_dispatch.hpp"
#include "config/shell_config.hpp"
#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "ui/design.hpp"
#include "ui/hit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#include <X11/keysym.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace eh::file_browser {
namespace xdg = eh::shell::desktop::xdg;

// Paint entry used by the buffer-release hook and configure handler below.
void draw_diskusage_window_impl(AppState& app);

namespace {

constexpr int kDuW = 1120;
constexpr int kDuH = 760;
constexpr int kPad = 16;

int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t du_hit(int ctrl) {
  return hui::Hit::dialog(hui::Hit::kDlgDiskUsage, ctrl);
}

} // namespace

namespace {

void on_du_buf_release_hook(void* user) {
  auto& app = *static_cast<AppState*>(user);
  if (!app.du_surface) return;
  if (app.du_pendingRedraw) {
    app.du_pendingRedraw = false;
    draw_diskusage_window_impl(app);
  }
}

void du_xdg_surface_configure(void* data, xdg_surface* surface,
                              uint32_t serial) {
  auto& app = *static_cast<AppState*>(data);
  xdg_surface_ack_configure(surface, serial);
  if (app.du_win_width <= 0 || app.du_win_height <= 0) return;
  bool needs_resize = (app.du_win_width != app.du_buf[0].width() ||
                       app.du_win_height != app.du_buf[0].height());
  if (needs_resize && app.shm) {
    app.du_buf[0].ensure(app.shm, "eh-du-a", app.du_win_width,
                         app.du_win_height);
    app.du_buf[1].ensure(app.shm, "eh-du-b", app.du_win_width,
                         app.du_win_height);
  }
  draw_diskusage_window_impl(app);
}

void du_toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h,
                           wl_array*) {
  auto& app = *static_cast<AppState*>(data);
  if (w > 0) app.du_win_width = w;
  if (h > 0) app.du_win_height = h;
}

void du_toplevel_close(void* data, xdg_toplevel*) {
  auto& app = *static_cast<AppState*>(data);
  close_disk_usage(app);
}

constexpr xdg_surface_listener kDuXdgSurfaceListener{
    .configure = du_xdg_surface_configure,
};

constexpr xdg_toplevel_listener kDuToplevelListener{
    .configure = du_toplevel_configure,
    .close = du_toplevel_close,
    .configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};


void du_status(AppState& app, const std::string& msg) {
  app.diskusage.status_msg = msg;
  app.diskusage.status_until_ms = steady_ms() + 4000;
}

} // namespace

// Rebuild the cached children of cur_dir (dirs from the aggregates map,
// files via binary range over the path-sorted vector).
void du_rebuild_kids(AppState& app) {
  auto& du = app.diskusage;
  du.dir_kids.clear();
  std::string base = du.cur_dir;
  if (!base.empty() && base.back() != '/') base += '/';
  for (auto& [dpath, agg] : du.dirs) {
    if (dpath.size() <= base.size()) continue;
    if (dpath.compare(0, base.size(), base) != 0) continue;
    std::string rest = dpath.substr(base.size());
    if (rest.empty() || rest.find('/') != std::string::npos) continue;
    DiskUsageState::DuChild k;
    k.path = dpath;
    k.name = rest;
    k.size = agg.bytes;
    k.is_dir = true;
    du.dir_kids.push_back(std::move(k));
  }
  {
    DiskFile lo{base, 0, {}};
    auto it = std::lower_bound(
        du.files.begin(), du.files.end(), lo,
        [](const DiskFile& a, const DiskFile& b) { return a.path < b.path; });
    for (; it != du.files.end(); ++it) {
      if (it->path.compare(0, base.size(), base) != 0) break;
      std::string rest = it->path.substr(base.size());
      if (rest.empty() || rest.find('/') != std::string::npos) continue;
      DiskUsageState::DuChild k;
      k.path = it->path;
      k.name = rest;
      k.size = it->size;
      k.is_dir = false;
      du.dir_kids.push_back(std::move(k));
    }
  }
  std::sort(du.dir_kids.begin(), du.dir_kids.end(),
            [](const DiskUsageState::DuChild& a,
               const DiskUsageState::DuChild& b) {
              return a.size > b.size;
            });
}

void draw_diskusage_dialog(AppState& app, cairo_t* cr);

void destroy_diskusage_window_impl(AppState& app) {
  if (!app.du_surface) return;
  for (auto& b : app.du_buf) b.destroy();
  if (app.du_toplevel) {
    xdg_toplevel_destroy(app.du_toplevel);
    app.du_toplevel = nullptr;
  }
  if (app.du_xdgSurface) {
    xdg_surface_destroy(app.du_xdgSurface);
    app.du_xdgSurface = nullptr;
  }
  wl_surface_destroy(app.du_surface);
  app.du_surface = nullptr;
  app.du_open = false;
  app.du_pendingRedraw = false;
}

void draw_diskusage_window_impl(AppState& app) {
  if (!app.du_surface || !app.du_open) return;
  int paint_bi = -1;
  for (int i = 0; i < 2; ++i) {
    if (!app.du_buf[i].busy()) {
      paint_bi = i;
      break;
    }
  }
  if (paint_bi < 0) {
    app.du_pendingRedraw = true;
    return;
  }
  int pw = app.du_win_width;
  int ph = app.du_win_height;
  cairo_t* cr = app.du_buf[paint_bi].cairo();
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  int saved_w = app.width;
  int saved_h = app.height;
  double saved_px = app.pointerX;
  double saved_py = app.pointerY;
  app.width = pw;
  app.height = ph;
  app.pointerX = static_cast<double>(app.du_pointerX);
  app.pointerY = static_cast<double>(app.du_pointerY);
  app.hit_diskusage.clear();

  draw_diskusage_dialog(app, cr);

  app.width = saved_w;
  app.height = saved_h;
  app.pointerX = saved_px;
  app.pointerY = saved_py;
  cairo_restore(cr);
  cairo_surface_flush(app.du_buf[paint_bi].cairo_surface());
  wl_surface_attach(app.du_surface, app.du_buf[paint_bi].wl(), 0, 0);
  wl_surface_damage_buffer(app.du_surface, 0, 0, pw, ph);
  app.du_buf[paint_bi].mark_busy();
  wl_surface_commit(app.du_surface);
  if (app.wl.display()) wl_display_flush(app.wl.display());
}

void draw_diskusage_window(AppState& app) {
  draw_diskusage_window_impl(app);
}

bool disk_usage_open(const AppState& app) { return app.du_open; }

void open_disk_usage(AppState& app, const std::string& root) {
  if (root.empty()) return;
  if (app.du_open) close_disk_usage(app);
  if (!app.du_surface) {
    auto* display = app.wl.display();
    auto* comp = app.wl.compositor();
    auto* xdg = app.wl.xdg_base();
    if (!display || !comp || !xdg || !app.shm) return;
    app.du_surface = wl_compositor_create_surface(comp);
    if (!app.du_surface) return;
    app.du_xdgSurface = xdg_wm_base_get_xdg_surface(xdg, app.du_surface);
    if (!app.du_xdgSurface) {
      wl_surface_destroy(app.du_surface);
      app.du_surface = nullptr;
      return;
    }
    xdg_surface_add_listener(app.du_xdgSurface, &kDuXdgSurfaceListener, &app);
    app.du_toplevel = xdg_surface_get_toplevel(app.du_xdgSurface);
    if (!app.du_toplevel) {
      xdg_surface_destroy(app.du_xdgSurface);
      app.du_xdgSurface = nullptr;
      wl_surface_destroy(app.du_surface);
      app.du_surface = nullptr;
      return;
    }
    xdg_toplevel_add_listener(app.du_toplevel, &kDuToplevelListener, &app);
    xdg_toplevel_set_title(app.du_toplevel, "Disk Usage");
    xdg_toplevel_set_app_id(app.du_toplevel, "horizon-files-diskusage");
    app.du_win_width = kDuW;
    app.du_win_height = kDuH;
    // Resizable (min only): paint derives every rect from the live size and
    // the treemap re-lays when its content rect moves (see paint).
    xdg_toplevel_set_min_size(app.du_toplevel, 960, 640);
    app.du_buf[0].ensure(app.shm, "eh-du-a", kDuW, kDuH);
    app.du_buf[1].ensure(app.shm, "eh-du-b", kDuW, kDuH);
    app.du_buf[0].set_release_hook(on_du_buf_release_hook, &app);
    app.du_buf[1].set_release_hook(on_du_buf_release_hook, &app);
    wl_surface_commit(app.du_surface);
  }
  auto& du = app.diskusage;
  du.root = root;
  du.cur_dir = root;
  du.treemap_root = root;
  du.selected.clear();
  du.highlight_ext.clear();
  du.scroll_dir = du.scroll_ext = 0;
  du.last_click_ms = 0;
  du.last_click_path.clear();
  du.status_msg.clear();
  du.settings_open = false;
  du.slider_drag = 0;
  // Panel opacities from the DU-local file (never the file-manager config).
  {
    eh::config::DiskUsageSettings s = eh::config::read_disk_usage_toml();
    app.du_drives_opacity_pct = s.drives_opacity_pct;
    app.du_dir_opacity_pct = s.dir_opacity_pct;
    app.du_ext_opacity_pct = s.ext_opacity_pct;
    app.du_map_opacity_pct = s.map_opacity_pct;
    app.du_bg_opacity_pct = s.bg_opacity_pct;
  }
  app.du_open = true;
  app.du_pendingRedraw = true;
  du_start_scan(app);
  draw_diskusage_window(app);
}

void close_disk_usage(AppState& app) {
  // Persist any panel edits (e.g. closed mid-drag via the × button).
  du_end_slider_drag(app);
  du_stop_dup_scan(app);
  du_stop_dup_trash(app);
  du_stop_scan(app);
  destroy_diskusage_window_impl(app);
}


namespace {

std::string du_human(uint64_t bytes) { return format_size(bytes); }

std::string du_pct(uint64_t part, uint64_t whole) {
  if (whole == 0) return "";
  double p = 100.0 * static_cast<double>(part) /
             static_cast<double>(whole);
  char buf[16];
  if (p >= 10.0)
    snprintf(buf, sizeof(buf), "%.1f%%", p);
  else
    snprintf(buf, sizeof(buf), "%.2f%%", p);
  return buf;
}

void du_button(AppState& app, cairo_t* cr, int ctrl, const char* label,
               int x, int y, int w, int h, bool enabled = true) {
  app.hit_diskusage.add(du_hit(ctrl), x, y, w, h);
  bool hov = enabled && (app.pointerX >= x && app.pointerX < x + w &&
                         app.pointerY >= y && app.pointerY < y + h);
  hui::design::button(cr, app, x, y, w, h, label, enabled, hov);
}

void du_section(AppState& app, cairo_t* cr, int x, int y, const char* label) {
  (void)app;
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 0.75);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 11);
  std::string up = label;
  for (auto& c : up)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  cairo_move_to(cr, x, y + 11);
  cairo_show_text(cr, up.c_str());
}

// Panel card: subtly varied background tone per region + a 1px hairline so
// adjacent panels read as distinct regions, not whitespace gaps.
// op_pct (0-100) is the Appearance-tab per-area opacity.
void du_panel(AppState& app, cairo_t* cr, int x, int y, int w, int h,
              double tone, int op_pct) {
  double alpha = tone * std::clamp(op_pct, 0, 100) / 100.0;
  hui::design::card_fill(cr, app, alpha);
  draw_rounded_rect(cr, x, y, w, h, 10);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.12 * std::clamp(op_pct, 0, 100) / 100.0 + 0.02);
  cairo_set_line_width(cr, 1.0);
  draw_rounded_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, 10);
  cairo_stroke(cr);
}

// Usage bar with a visible dark-groove track (the shared bar() track is
// near-black and vanishes on dark themes, reading as a stray line).
void du_track_bar(AppState& app, cairo_t* cr, double x, double y, double w,
                  double frac, double h) {
  if (w <= 0 || h <= 0) return;
  frac = std::clamp(frac, 0.0, 1.0);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
  draw_rounded_rect(cr, x, y, w, h, h / 2);
  cairo_fill(cr);
  if (frac > 0.001) {
    bool hot = frac > 0.9;
    if (hot)
      cairo_set_source_rgba(cr, 0.95, 0.32, 0.30, 0.95);
    else
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                            0.92);
    draw_rounded_rect(cr, x, y, std::max(2.0, w * frac), h, h / 2);
    cairo_fill(cr);
  }
}

// Capacity bar with intentional warn thresholds (drives only): green below
// 70%, amber 70-90%, red above. Fraction bars elsewhere stay neutral
// accent — a proportion is not a warning.
void du_capacity_bar(AppState& app, cairo_t* cr, double x, double y, double w,
                     double frac, double h) {
  if (w <= 0 || h <= 0) return;
  frac = std::clamp(frac, 0.0, 1.0);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
  draw_rounded_rect(cr, x, y, w, h, h / 2);
  cairo_fill(cr);
  if (frac > 0.001) {
    if (frac >= 0.9)
      cairo_set_source_rgba(cr, 0.95, 0.35, 0.30, 0.95);
    else if (frac >= 0.7)
      cairo_set_source_rgba(cr, 0.95, 0.70, 0.20, 0.95);
    else
      cairo_set_source_rgba(cr, 0.32, 0.75, 0.45, 0.95);
    draw_rounded_rect(cr, x, y, std::max(2.0, w * frac), h, h / 2);
    cairo_fill(cr);
  }
}

// Thin scrollbar groove + thumb for the list panels. Only paints (and
// registers) on overflow; drag math in du_apply_scroll_drag mirrors the
// geometry below exactly.
void du_scrollbar(AppState& app, cairo_t* cr, int ctrl, const int* rect,
                  int content_h, int scroll) {
  int rx = rect[0], ry = rect[1], rw = rect[2], rh = rect[3];
  if (content_h <= rh || rh <= 0) return;
  cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
  draw_rounded_rect(cr, rx + rw - 8, ry + 4, 5, rh - 8, 2.5);
  cairo_fill(cr);
  double th = std::max(24.0, static_cast<double>(rh) * rh / content_h);
  double travel = (rh - 8) - th;
  double frac = (content_h > rh) ? static_cast<double>(scroll) /
                                       static_cast<double>(content_h - rh)
                                 : 0;
  double ty = ry + 4 + std::clamp(frac, 0.0, 1.0) * (travel > 0 ? travel : 0);
  app.hit_diskusage.add(du_hit(ctrl), rx + rw - 13, ry, 13, rh);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.28);
  draw_rounded_rect(cr, rx + rw - 8, ty, 5, th, 2.5);
  cairo_fill(cr);
}


// Small-caps column header, right-aligned at right_x (or left at x).
void du_colheader(AppState& app, cairo_t* cr, const char* label, double x,
                  double right_x, double y) {
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 0.8);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 10.5);
  if (right_x > x) {
    cairo_text_extents_t te;
    cairo_text_extents(cr, label, &te);
    cairo_move_to(cr, right_x - te.x_advance, y);
  } else {
    cairo_move_to(cr, x, y);
  }
  cairo_show_text(cr, label);
}

} // namespace

void du_apply_scroll_drag(AppState& app, int y) {
  auto& du = app.diskusage;
  const int* r = nullptr;
  int content = 0;
  int* scroll = nullptr;
  if (du.scroll_drag == 1) {
    r = du.dir_rect;
    content = du.dup_mode ? du.dup_content_h : du.dir_content_h;
    scroll = du.dup_mode ? &du.dup_scroll : &du.scroll_dir;
  } else if (du.scroll_drag == 2) {
    r = du.ext_rect;
    content = du.ext_content_h;
    scroll = &du.scroll_ext;
  } else {
    return;
  }
  if (content <= r[3] || r[3] <= 0) return;
  double th = std::max(24.0, static_cast<double>(r[3]) * r[3] / content);
  double travel = (r[3] - 8) - th;
  if (travel <= 0) return;
  double frac = (static_cast<double>(y) - (r[1] + 4) - th / 2) / travel;
  *scroll = std::clamp(static_cast<int>(frac * (content - r[3])), 0,
                       content - r[3]);
  app.du_pendingRedraw = true;
}

// CSV report: summary + directories + extensions + duplicates tables.
// Pure (no IO) so probes can verify the exact bytes.
std::string du_build_report_csv(const AppState& app) {
  const auto& du = app.diskusage;
  auto esc = [](const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
      if (c == '"') o += "\"\"";
      else o += c;
    }
    o += "\"";
    return o;
  };
  std::string out;
  char tbuf[64];
  {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
  }
  out += "# Horizon Disk Usage report\n";
  out += "# root: " + du.root + "\n";
  out += std::string("# date: ") + tbuf + "\n";
  out += "summary,files,folders,bytes,unreadable,skipped\n";
  {
    char b[128];
    snprintf(b, sizeof(b), "total,%llu,%llu,%llu,%llu,%llu\n",
             (unsigned long long)du.total_files,
             (unsigned long long)du.total_dirs,
             (unsigned long long)du.total_bytes,
             (unsigned long long)du.unknown_dirs,
             (unsigned long long)du.pruned_dirs);
    out += b;
  }
  out += "directories\npath,bytes,files\n";
  {
    std::vector<std::pair<std::string, DiskDir>> rows(du.dirs.begin(),
                                                      du.dirs.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      return a.second.bytes > b.second.bytes;
    });
    for (auto& [p, d] : rows) {
      char b[128];
      snprintf(b, sizeof(b), "%s,%llu,%llu\n", esc(p).c_str(),
               (unsigned long long)d.bytes, (unsigned long long)d.files);
      out += b;
    }
  }
  out += "extensions\nextension,bytes,files,percent\n";
  for (auto& e : du.exts) {
    char b[160];
    snprintf(b, sizeof(b), "%s,%llu,%llu,%s\n",
             esc(e.ext.empty() ? "(no extension)" : "." + e.ext).c_str(),
             (unsigned long long)e.bytes, (unsigned long long)e.files,
             du_pct(e.bytes, du.total_bytes).c_str());
    out += b;
  }
  if (du.dup_done && du.dup_data_gen == du.du_data_gen &&
      !du.dup_groups.empty()) {
    out += "duplicates\nsize_bytes,hash16,file_count,paths\n";
    for (auto& g : du.dup_groups) {
      char b[128];
      snprintf(b, sizeof(b), "%llu,%016llx,%zu,", (unsigned long long)g.size,
               (unsigned long long)g.hash, g.paths.size());
      out += b;
      std::string joined;
      for (size_t i = 0; i < g.paths.size(); ++i) {
        if (i > 0) joined += ";";
        joined += g.paths[i];
      }
      out += esc(joined) + "\n";
    }
  } else {
    out += "# duplicates: not scanned (toggle Duplicates first)\n";
  }
  return out;
}

// Breadcrumb segments (label + drill path) for the toolbar. Pure.
void du_build_crumbs(AppState& app) {
  auto& du = app.diskusage;
  du.crumb_labels.clear();
  du.crumb_paths.clear();
  std::string root = du.root.empty() ? "/" : du.root;
  std::string cur = du.cur_dir.empty() ? root : du.cur_dir;
  // Root segment: sidebar drive label when known, else basename.
  std::string root_label;
  for (auto& loc : app.sidebar_locations) {
    if ((loc.kind == SidebarLocation::Kind::Drive ||
         loc.kind == SidebarLocation::Kind::Root) &&
        loc.path == root) {
      root_label = loc.label;
      break;
    }
  }
  if (root_label.empty()) {
    auto slash = root.rfind('/');
    root_label = (slash == std::string::npos || slash + 1 >= root.size())
                     ? root
                     : root.substr(slash + 1);
    if (root_label.empty()) root_label = "/";
  }
  du.crumb_labels.push_back(root_label);
  du.crumb_paths.push_back(root);
  if (cur.size() > root.size() && cur.compare(0, root.size(), root) == 0 &&
      (root == "/" || cur[root.size()] == '/')) {
    std::string rel = cur.substr(root.size());
    std::string acc = root;
    size_t pos = 0;
    while (pos < rel.size()) {
      while (pos < rel.size() && rel[pos] == '/') ++pos;
      if (pos >= rel.size()) break;
      size_t end = rel.find('/', pos);
      if (end == std::string::npos) end = rel.size();
      acc += "/" + rel.substr(pos, end - pos);
      // Collapse the "//x" seam when root itself is "/".
      if (acc.size() > 2 && acc[0] == '/' && acc[1] == '/')
        acc.erase(0, 1);
      du.crumb_labels.push_back(rel.substr(pos, end - pos));
      du.crumb_paths.push_back(acc);
      if (du.crumb_labels.size() >= 24) break;
      pos = end;
    }
  }
}

// Duplicates view: same panel as the directory list (toggle). Groups with
// the most waste first; files selectable with treemap sync; group trash
// (keep first per group) via the header button with armed confirm.
void du_paint_dups(AppState& app, cairo_t* cr, int dir_x, int list_top,
                   int dir_w, int list_h, int body_y) {
  auto& du = app.diskusage;
  const bool hashing = du.dup_scanning.load(std::memory_order_relaxed);
  const bool trashing = du.dupact_running.load(std::memory_order_relaxed);
  char title[160];
  if (hashing) {
    snprintf(title, sizeof(title), "Duplicates — hashing…");
  } else if (trashing) {
    snprintf(title, sizeof(title), "Duplicates — trashing…");
  } else if (du.dup_done) {
    snprintf(title, sizeof(title), "Duplicates — %zu group%s · %s wasted",
             du.dup_groups.size(), du.dup_groups.size() == 1 ? "" : "s",
             du_human(du.dup_wasted).c_str());
  } else {
    snprintf(title, sizeof(title), "Duplicates");
  }
  du_section(app, cr, dir_x, body_y, title);
  // Trash-all button (two-step armed confirm; label tells the state).
  {
    bool can = !du.dup_groups.empty() && !hashing && !trashing;
    const char* label =
        du.dup_confirm_armed ? "Click again!" : "Trash dupes";
    du_button(app, cr, hui::Hit::kDuDupTrashAll, label, dir_x + dir_w - 150,
              body_y - 4, 140, 22, can);
  }
  const int hdr_h = 20;
  const int rows_top = list_top + 6 + hdr_h;
  const int rows_h = list_h - 12 - hdr_h;
  du.dir_rect[0] = dir_x;
  du.dir_rect[1] = rows_top;
  du.dir_rect[2] = dir_w;
  du.dir_rect[3] = rows_h;
  du_colheader(app, cr, "NAME", dir_x + 10, 0, list_top + 18);
  du_colheader(app, cr, "FILES", 0, dir_x + dir_w - 256, list_top + 18);
  du_colheader(app, cr, "SIZE", 0, dir_x + dir_w - 176, list_top + 18);
  du_colheader(app, cr, "%", 0, dir_x + dir_w - 118, list_top + 18);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.09);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, dir_x, list_top + 24.5);
  cairo_line_to(cr, dir_x + dir_w, list_top + 24.5);
  cairo_stroke(cr);

  cairo_save(cr);
  cairo_rectangle(cr, dir_x, rows_top, dir_w, rows_h);
  cairo_clip(cr);
  auto center_note = [&](const char* msg) {
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_text_extents_t te;
    cairo_text_extents(cr, msg, &te);
    cairo_move_to(cr, dir_x + (dir_w - te.x_advance) / 2,
                  rows_top + rows_h / 2);
    cairo_show_text(cr, msg);
  };
  if (hashing) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Hashing… %llu / %llu files",
             (unsigned long long)du.dup_p_done.load(),
             (unsigned long long)du.dup_p_total.load());
    center_note(buf);
    double frac = du.dup_p_total.load() > 0
                      ? static_cast<double>(du.dup_p_done.load()) /
                            static_cast<double>(du.dup_p_total.load())
                      : 0;
    du_track_bar(app, cr, dir_x + 40, rows_top + rows_h / 2 + 16, dir_w - 80,
                 frac, 5.0);
    du.dup_content_h = 0;
    cairo_restore(cr);
    return;
  }
  if (trashing) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Trashing… %llu / %llu",
             (unsigned long long)du.dupact_done.load(),
             (unsigned long long)du.dupact_total.load());
    center_note(buf);
    du.dup_content_h = 0;
    cairo_restore(cr);
    return;
  }
  if (!du.dup_done) {
    center_note("Nothing to hash yet — wait for the drive scan.");
    du.dup_content_h = 0;
    cairo_restore(cr);
    return;
  }
  if (du.dup_groups.empty()) {
    center_note("No duplicates found.");
    du.dup_content_h = 0;
    cairo_restore(cr);
    return;
  }
  const int row_h = 26;
  int y = rows_top - du.dup_scroll;
  int rows = 0;
  du.hover_row = -1;
  size_t shown_groups = 0;
  for (auto& r : du.dup_rows) {
    if (rows >= 128) break; // hit-id range cap (same as directory rows)
    if (r.header) shown_groups = r.group + 1;
    int ry = y + rows * row_h;
    if (ry + row_h >= rows_top && ry < rows_top + rows_h) {
      const auto& g = du.dup_groups[r.group];
      bool sel = false;
      if (r.header) {
        sel = (!g.paths.empty() && g.paths[0] == du.selected);
      } else {
        sel = (g.paths[r.member] == du.selected);
      }
      if ((rows & 1) == 0 && !sel) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.022);
        cairo_rectangle(cr, dir_x, ry, dir_w, row_h);
        cairo_fill(cr);
      }
      bool hov = (app.pointerX >= dir_x && app.pointerX < dir_x + dir_w &&
                  app.pointerY >= ry && app.pointerY < ry + row_h);
      if (hov) du.hover_row = rows;
      if (hov && !sel) {
        hui::design::row_hover(cr, app, dir_x, ry, dir_w, row_h - 2);
      }
      if (sel) {
        hui::design::card_fill(cr, app, 0.8);
        draw_rounded_rect(cr, dir_x, ry, dir_w, row_h - 2, 7);
        cairo_fill(cr);
      }
      app.hit_diskusage.add(du_hit(hui::Hit::kDuDirRowBase + rows), dir_x,
                            ry, dir_w, row_h);
      auto right_text = [&](const std::string& s, double right_x) {
        cairo_text_extents_t te;
        cairo_text_extents(cr, s.c_str(), &te);
        cairo_move_to(cr, right_x - te.x_advance, ry + 18);
        cairo_show_text(cr, s.c_str());
      };
      if (r.header) {
        if (r.group >= shown_groups) shown_groups = r.group + 1;
        char hbuf[128];
        uint64_t wasted = (g.paths.size() - 1) * g.size;
        snprintf(hbuf, sizeof(hbuf), "%zu × %s — wasted %s", g.paths.size(),
                 du_human(g.size).c_str(), du_human(wasted).c_str());
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                              sel ? 1.0 : 0.95);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12.5);
        cairo_move_to(cr, dir_x + 8, ry + 18);
        cairo_show_text(
            cr, hui::design::clip_end(cr, hbuf, dir_w - 300).c_str());
        right_text(du_pct(wasted, du.dup_wasted ? du.dup_wasted : 1),
                   dir_x + dir_w - 118);
        right_text(du_human(g.size), dir_x + dir_w - 176);
      } else {
        auto slash = g.paths[r.member].rfind('/');
        std::string nm = (slash == std::string::npos)
                             ? g.paths[r.member]
                             : g.paths[r.member].substr(slash + 1);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                              sel ? 1.0 : 0.9);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.5);
        cairo_move_to(cr, dir_x + 24, ry + 18);
        cairo_show_text(
            cr, hui::design::clip_end(cr, nm, dir_w - 316).c_str());
        cairo_set_source_rgba(cr, app.text_secondary_r,
                              app.text_secondary_g, app.text_secondary_b, 1.0);
        cairo_set_font_size(cr, 12);
        char ibuf[32];
        snprintf(ibuf, sizeof(ibuf), "%zu/%zu", r.member + 1,
                 g.paths.size());
        right_text(ibuf, dir_x + dir_w - 256);
        right_text(du_human(g.size), dir_x + dir_w - 176);
        uint64_t wasted = (g.paths.size() - 1) * g.size;
        right_text(du_pct(wasted, du.dup_wasted ? du.dup_wasted : 1),
                   dir_x + dir_w - 118);
      }
      cairo_set_source_rgba(cr, 1, 1, 1, 0.055);
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, dir_x + 8, ry + row_h - 0.5);
      cairo_line_to(cr, dir_x + dir_w - 8, ry + row_h - 0.5);
      cairo_stroke(cr);
    }
    ++rows;
  }
  du.dup_content_h = rows * row_h;
  // Footer when the row cap hides groups.
  if (shown_groups < du.dup_groups.size()) {
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
    cairo_set_font_size(cr, 12);
    char fbuf[96];
    snprintf(fbuf, sizeof(fbuf), "…and %zu more groups",
             du.dup_groups.size() - shown_groups);
    cairo_move_to(cr, dir_x + 8, rows_top + rows_h - 8);
    cairo_show_text(cr, fbuf);
  }
  cairo_restore(cr);
  du_scrollbar(app, cr, hui::Hit::kDuDirScroll, du.dir_rect, du.dup_content_h,
               du.dup_scroll);
}

void draw_diskusage_dialog(AppState& app, cairo_t* cr) {
  auto& du = app.diskusage;
  const int W = app.width;
  const int H = app.height;
  // Window background (own opacity slider, not the file-manager surface).
  hui::design::card_fill(
      cr, app, std::clamp(app.du_bg_opacity_pct, 0, 100) / 100.0);
  cairo_rectangle(cr, 0, 0, W, H);
  cairo_fill(cr);

  const int pad = kPad;
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 14);
  cairo_move_to(cr, pad, 28);
  cairo_show_text(cr, "Disk Usage");
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  std::string sub = hui::design::clip_end(cr, du.root, W - 220);
  cairo_text_extents_t te;
  cairo_text_extents(cr, "Disk Usage  ", &te);
  cairo_move_to(cr, pad + te.x_advance + 90, 28);
  cairo_show_text(cr, ("—  " + sub).c_str());
  app.hit_diskusage.add(du_hit(hui::Hit::kDuClose), W - 40, 10, 24, 24);
  {
    bool hov = (app.pointerX >= W - 40 && app.pointerX < W - 16 &&
                app.pointerY >= 10 && app.pointerY < 34);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                          hov ? 1.0 : 0.6);
    cairo_set_font_size(cr, 15);
    cairo_move_to(cr, W - 33, 28);
    cairo_show_text(cr, "×");
  }
  // Traffic lights (house style).
  {
    const double dots[3][3] = {{0.35, 0.85, 0.35},
                               {1.0, 0.75, 0.2},
                               {1.0, 0.35, 0.35}};
    for (int i = 0; i < 3; ++i) {
      cairo_new_path(cr);
      cairo_set_source_rgba(cr, dots[i][0], dots[i][1], dots[i][2], 1.0);
      cairo_arc(cr, W - 58 - i * 20, 20, 6, 0, 2 * M_PI);
      cairo_fill(cr);
    }
  }

  const int bar_y = 48, bar_h = 32;
  const bool scanning = du.scanning.load(std::memory_order_relaxed);
  du_button(app, cr, hui::Hit::kDuRescan, "Rescan", pad, bar_y, 90, 28,
            !scanning);
  du_button(app, cr, hui::Hit::kDuStop, "Stop", pad + 98, bar_y, 80, 28,
            scanning);
  du_button(app, cr, hui::Hit::kDuDuplicates, "Duplicates", pad + 186, bar_y,
            96, 28, !scanning && du.done);
  du_button(app, cr, hui::Hit::kDuReport, "Report", pad + 290, bar_y, 80, 28,
            !scanning && du.done);
  // Breadcrumb / progress center.
  {
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.5);
    std::string mid;
    if (scanning) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Scanning… %llu files · %s",
               (unsigned long long)du.p_files.load(),
               du_human(du.p_bytes.load()).c_str());
      mid = buf;
    } else if (du.dup_mode) {
      mid = "Duplicates";
    } else {
      mid.clear(); // crumbs paint below
    }
    int mid_w = std::max(80, W - 938);
    int mid_x = pad + 378;
    if (!mid.empty()) {
      std::string shown = hui::design::clip_middle(cr, mid, mid_w);
      cairo_move_to(cr, mid_x, bar_y + 21);
      cairo_show_text(cr, shown.c_str());
    } else if (!scanning && !du.dup_mode) {
      // Clickable drill breadcrumbs (drive label + path segments).
      du_build_crumbs(app);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12.5);
      struct SegW {
        std::string label;
        double w = 0;
      };
      std::vector<SegW> segs;
      double sep_w = 0;
      {
        cairo_text_extents_t se;
        cairo_text_extents(cr, " › ", &se);
        sep_w = se.x_advance;
      }
      for (auto& l : du.crumb_labels) {
        cairo_text_extents_t te;
        cairo_text_extents(cr, l.c_str(), &te);
        segs.push_back({l, te.x_advance});
      }
      // Overflow: keep the tail, collapse the head into "…".
      size_t first = 0;
      {
        double acc = 0;
        size_t keep = 0;
        for (size_t i = segs.size(); i-- > 0;) {
          double need = segs[i].w + (keep > 0 ? sep_w : 0);
          if (acc + need > mid_w) break;
          acc += need;
          ++keep;
        }
        if (keep < segs.size()) {
          // Room for "…" + separator if anything fits at all.
          first = segs.size() - keep;
        }
      }
      double cx = mid_x;
      if (first > 0) {
        cairo_set_source_rgba(cr, app.text_secondary_r,
                              app.text_secondary_g, app.text_secondary_b, 1.0);
        cairo_move_to(cr, cx, bar_y + 21);
        cairo_show_text(cr, "…");
        cairo_text_extents_t ee;
        cairo_text_extents(cr, "…", &ee);
        cx += ee.x_advance + sep_w;
      }
      for (size_t i = first; i < segs.size(); ++i) {
        bool hov = (app.pointerX >= cx && app.pointerX < cx + segs[i].w &&
                    app.pointerY >= bar_y && app.pointerY < bar_y + 28);
        if (i < 24) {
          app.hit_diskusage.add(
              du_hit(hui::Hit::kDuCrumbBase + static_cast<int>(i)),
              static_cast<int>(cx), bar_y, static_cast<int>(segs[i].w) + 1,
              28);
        }
        if (hov) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                                1.0);
        } else {
          cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        }
        cairo_move_to(cr, cx, bar_y + 21);
        cairo_show_text(cr, segs[i].label.c_str());
        cx += segs[i].w;
        if (i + 1 < segs.size()) {
          cairo_set_source_rgba(cr, app.text_secondary_r,
                                app.text_secondary_g, app.text_secondary_b,
                                1.0);
          cairo_move_to(cr, cx, bar_y + 21);
          cairo_show_text(cr, " › ");
          cx += sep_w;
        }
      }
    }
  }
  du_button(app, cr, hui::Hit::kDuUp, "Up", W - pad - 5 * 96 - 4 * 8, bar_y,
            80, 28, du.cur_dir != du.root && !du.cur_dir.empty());
  du_button(app, cr, hui::Hit::kDuZoom, "Zoom", W - pad - 4 * 96 - 3 * 8,
            bar_y, 80, 28, !du.selected.empty());
  du_button(app, cr, hui::Hit::kDuOpenLoc, "Open", W - pad - 3 * 96 - 2 * 8,
            bar_y, 80, 28, !du.selected.empty());
  du_button(app, cr, hui::Hit::kDuDelete, "Delete", W - pad - 2 * 96 - 8,
            bar_y, 80, 28, !du.selected.empty());
  du_button(app, cr, hui::Hit::kDuSettings, "Settings", W - pad - 96, bar_y,
            80, 28);

  {
    cairo_set_source_rgba(cr, 1, 1, 1, 0.09);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, pad, 84.5);
    cairo_line_to(cr, W - pad, 84.5);
    cairo_stroke(cr);
    const double sh[4] = {0.10, 0.06, 0.03, 0.01};
    for (int i = 0; i < 4; ++i) {
      cairo_set_source_rgba(cr, 0, 0, 0, sh[i]);
      cairo_rectangle(cr, pad, 86 + i, W - 2 * pad, 1);
      cairo_fill(cr);
    }
  }

  const int body_y = 98;
  // Fixed list height; clamped so the map + status always keep room.
  const int list_h = std::max(200, std::min(352, H - 288));
  const int list_top = body_y + 22;
  const int drives_w = 170;
  const int ext_w = 230;
  const int dir_x = pad + drives_w + 10;
  const int dir_w = W - pad - ext_w - 10 - dir_x;
  const int ext_x = W - pad - ext_w;
  // Panel cards with distinct tones: drives / directory / extensions.
  du_panel(app, cr, pad - 6, list_top - 6, drives_w + 12, list_h + 12, 0.62,
             app.du_drives_opacity_pct);
  du_panel(app, cr, dir_x - 6, list_top - 6, dir_w + 12, list_h + 12, 0.45,
             app.du_dir_opacity_pct);
  du_panel(app, cr, ext_x - 6, list_top - 6, ext_w + 12, list_h + 12, 0.56,
             app.du_ext_opacity_pct);
  du.dir_rect[0] = dir_x;
  du.dir_rect[1] = list_top + 26;
  du.dir_rect[2] = dir_w;
  du.dir_rect[3] = list_h - 32;
  du.ext_rect[0] = ext_x;
  du.ext_rect[1] = list_top;
  du.ext_rect[2] = ext_w;
  du.ext_rect[3] = list_h;

  // Drives mini-list.
  du_section(app, cr, pad, body_y, "Drives");
  {
    cairo_save(cr);
    cairo_rectangle(cr, pad - 6, list_top - 6, drives_w + 12, list_h + 12);
    cairo_clip(cr);
    const int row_step = 52;
    int dy = list_top + 6;
    int di = 0;
    for (auto& loc : app.sidebar_locations) {
      // Local disks plus the root filesystem row (Kind::Root, "/").
      // NOTE: the drive-switch click handler below uses this same filter
      // and order — keep them in sync so hit indices line up.
      if (loc.kind != SidebarLocation::Kind::Drive &&
          loc.kind != SidebarLocation::Kind::Root)
        continue;
      if (loc.path.empty() || loc.path[0] != '/') continue;
      if (di >= 16) break;
      bool sel = (loc.path == du.root);
      int ry = dy + di * row_step;
      if (ry + row_step >= list_top - 6 && ry < list_top + list_h + 6) {
        if (sel) {
          hui::design::card_fill(cr, app, 0.8);
          draw_rounded_rect(cr, pad - 2, ry, drives_w + 4, row_step - 4, 7);
          cairo_fill(cr);
        }
        app.hit_diskusage.add(du_hit(hui::Hit::kDuDriveRowBase + di), pad - 2,
                              ry, drives_w + 4, row_step - 4);
        // Line 1: place name, full width — never squeezed by the numbers.
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                              sel ? 1.0 : 0.85);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.5);
        cairo_move_to(cr, pad + 4, ry + 14);
        std::string shown =
            hui::design::clip_end(cr, loc.label, drives_w - 12);
        cairo_show_text(cr, shown.c_str());
        // Line 2: capacity left, grooved bar below spanning the width.
        if (loc.total_bytes > 0) {
          uint64_t used = loc.total_bytes > loc.free_bytes
                              ? loc.total_bytes - loc.free_bytes
                              : 0;
          std::string cap =
              du_human(used) + " / " + du_human(loc.total_bytes);
          cairo_set_source_rgba(cr, app.text_secondary_r,
                                app.text_secondary_g, app.text_secondary_b,
                                1.0);
          cairo_set_font_size(cr, 10.5);
          cairo_move_to(cr, pad + 4, ry + 27);
          cairo_show_text(cr, cap.c_str());
          double frac = 0;
          if (loc.free_bytes < loc.total_bytes)
            frac = static_cast<double>(loc.total_bytes - loc.free_bytes) /
                   static_cast<double>(loc.total_bytes);
          du_capacity_bar(app, cr, pad + 4, ry + 32, drives_w - 8, frac,
                          4.0);
        }
      }
      ++di;
    }
    cairo_restore(cr);
  }

  // Directory list, or the duplicates view in dup mode (same
  // panel, same hit-id family; the handler branches on dup_mode).
  if (du.dup_mode) {
    du_paint_dups(app, cr, dir_x, list_top, dir_w, list_h, body_y);
  } else {
    du_section(app, cr, dir_x, body_y, "Directory list");
    {
      const int hdr_h = 20;
      const int rows_top = list_top + 6 + hdr_h;
      const int rows_h = list_h - 12 - hdr_h;
      // Fixed column header: NAME | FILES | SIZE | %.
      du_colheader(app, cr, "NAME", dir_x + 10, 0, list_top + 18);
      du_colheader(app, cr, "FILES", 0, dir_x + dir_w - 256, list_top + 18);
      du_colheader(app, cr, "SIZE", 0, dir_x + dir_w - 176, list_top + 18);
      du_colheader(app, cr, "%", 0, dir_x + dir_w - 118, list_top + 18);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.09);
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, dir_x, list_top + 24.5);
      cairo_line_to(cr, dir_x + dir_w, list_top + 24.5);
      cairo_stroke(cr);

      cairo_save(cr);
      cairo_rectangle(cr, dir_x, rows_top, dir_w, rows_h);
      cairo_clip(cr);
      uint64_t cur_total = 0;
      {
        auto it = du.dirs.find(du.cur_dir);
        if (it != du.dirs.end()) cur_total = it->second.bytes;
      }
      if (cur_total == 0) {
        for (auto& k : du.dir_kids) cur_total += k.size;
      }
      const int row_h = 30;
      int y = rows_top - du.scroll_dir;
      int rows = 0;
      du.hover_row = -1;
      for (auto& k : du.dir_kids) {
        if (rows >= 128) break;
        int ry = y + rows * row_h;
        if (ry + row_h >= rows_top && ry < rows_top + rows_h) {
          bool sel = (k.path == du.selected);
          bool hov = (app.pointerX >= dir_x && app.pointerX < dir_x + dir_w &&
                      app.pointerY >= ry && app.pointerY < ry + row_h);
          if (hov) du.hover_row = rows;
          // Zebra striping, then hover pill, then selection pill on top.
          if ((rows & 1) == 0 && !sel) {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.022);
            cairo_rectangle(cr, dir_x, ry, dir_w, row_h);
            cairo_fill(cr);
          }
          if (hov && !sel) {
            hui::design::row_hover(cr, app, dir_x, ry, dir_w, row_h - 2);
          }
          if (sel) {
            hui::design::card_fill(cr, app, 0.8);
            draw_rounded_rect(cr, dir_x, ry, dir_w, row_h - 2, 7);
            cairo_fill(cr);
          }
          app.hit_diskusage.add(
              du_hit(hui::Hit::kDuDirRowBase + rows), dir_x, ry, dir_w, row_h);
          cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                                sel ? 1.0 : 0.9);
          cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                 CAIRO_FONT_WEIGHT_NORMAL);
          cairo_set_font_size(cr, 12.5);
          cairo_move_to(cr, dir_x + 8, ry + 19);
          std::string shown =
              hui::design::clip_end(cr, k.name, dir_w - 300);
          cairo_show_text(cr, shown.c_str());
          // FILES (dirs: subtree file count) · SIZE · % + bar, right side.
          cairo_set_source_rgba(cr, app.text_secondary_r,
                                app.text_secondary_g, app.text_secondary_b, 1.0);
          cairo_set_font_size(cr, 12);
          auto right_text = [&](const std::string& s, double right_x) {
            cairo_text_extents_t te;
            cairo_text_extents(cr, s.c_str(), &te);
            cairo_move_to(cr, right_x - te.x_advance, ry + 19);
            cairo_show_text(cr, s.c_str());
          };
          if (k.is_dir) {
            auto it = du.dirs.find(k.path);
            uint64_t nf = (it != du.dirs.end()) ? it->second.files : 0;
            char fbuf[32];
            snprintf(fbuf, sizeof(fbuf), "%llu",
                     (unsigned long long)nf);
            right_text(fbuf, dir_x + dir_w - 256);
          } else {
            right_text("—", dir_x + dir_w - 256);
          }
          std::string sz = du_human(k.size);
          right_text(sz, dir_x + dir_w - 176);
          std::string pct = du_pct(k.size, cur_total);
          right_text(pct, dir_x + dir_w - 118);
          double frac = cur_total > 0
                            ? static_cast<double>(k.size) /
                                  static_cast<double>(cur_total)
                            : 0;
          du_track_bar(app, cr, dir_x + dir_w - 108, ry + 8, 100, frac, 5.0);
          // Row separator.
          cairo_set_source_rgba(cr, 1, 1, 1, 0.055);
          cairo_set_line_width(cr, 1.0);
          cairo_move_to(cr, dir_x + 8, ry + row_h - 0.5);
          cairo_line_to(cr, dir_x + dir_w - 8, ry + row_h - 0.5);
          cairo_stroke(cr);
        }
        ++rows;
      }
      du.dir_content_h = rows * row_h;
      cairo_restore(cr);
      du_scrollbar(app, cr, hui::Hit::kDuDirScroll, du.dir_rect,
                   du.dir_content_h, du.scroll_dir);
    }


  }
  du_section(app, cr, ext_x, body_y, "Extension list");
  {
    cairo_save(cr);
    cairo_rectangle(cr, ext_x, body_y + 22, ext_w, list_h);
    cairo_clip(cr);
    const int row_h = 26;
    int y = body_y + 22 - du.scroll_ext;
    int rows = 0;
    for (auto& e : du.exts) {
      if (rows >= 64) break;
      int ry = y + rows * row_h;
      if (ry + row_h >= body_y + 22 && ry < body_y + 22 + list_h) {
        bool hl = (!du.highlight_ext.empty() && du.highlight_ext == e.ext);
        if (hl) {
          hui::design::card_fill(cr, app, 0.6);
          draw_rounded_rect(cr, ext_x, ry, ext_w, row_h - 2, 6);
          cairo_fill(cr);
        }
        app.hit_diskusage.add(du_hit(hui::Hit::kDuExtRowBase + rows), ext_x,
                              ry, ext_w, row_h);
        cairo_new_path(cr);
        cairo_set_source_rgba(cr, e.r, e.g, e.b, 1.0);
        cairo_arc(cr, ext_x + 10, ry + row_h / 2.0, 5.0, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12);
        cairo_move_to(cr, ext_x + 22, ry + 17);
        std::string label = e.ext.empty() ? "(no extension)" : "." + e.ext;
        cairo_show_text(
            cr, hui::design::clip_end(cr, label, 92).c_str());
        // Mini-bar with a full-width track so small shares read as
        // proportions, not broken stubs. Fill uses the legend color.
        {
          double frac = du.total_bytes > 0
                            ? static_cast<double>(e.bytes) /
                                  static_cast<double>(du.total_bytes)
                            : 0;
          frac = std::clamp(frac, 0.0, 1.0);
          const double bx = ext_x + 120, bw = 48;
          cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
          draw_rounded_rect(cr, bx, ry + 8, bw, 5.0, 2.5);
          cairo_fill(cr);
          if (frac > 0.001) {
            cairo_set_source_rgba(cr, e.r, e.g, e.b, 0.9);
            draw_rounded_rect(cr, bx, ry + 8, std::max(2.0, bw * frac), 5.0,
                              2.5);
            cairo_fill(cr);
          }
        }
        std::string pct = du_pct(e.bytes, du.total_bytes);
        cairo_set_source_rgba(cr, app.text_secondary_r,
                              app.text_secondary_g, app.text_secondary_b, 1.0);
        cairo_text_extents_t pe;
        cairo_text_extents(cr, pct.c_str(), &pe);
        cairo_move_to(cr, ext_x + ext_w - 8 - pe.x_advance, ry + 17);
        cairo_show_text(cr, pct.c_str());
        // Row divider so dot → label → % traces without drift.
        cairo_set_source_rgba(cr, 1, 1, 1, 0.055);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, ext_x + 8, ry + row_h - 0.5);
        cairo_line_to(cr, ext_x + ext_w - 8, ry + row_h - 0.5);
        cairo_stroke(cr);
      }
      ++rows;
    }
    du.ext_content_h = rows * row_h;
    cairo_restore(cr);
    du_scrollbar(app, cr, hui::Hit::kDuExtScroll, du.ext_rect,
                 du.ext_content_h, du.scroll_ext);
  }

  // Geometry is final in du.tiles (see kDuMap*): paint only reads it.
  const int map_y = list_top + list_h + 8;
  const int map_top = map_y + 22;
  const int map_h = H - 40 - map_top;
  du_section(app, cr, pad, map_y, "Treemap");
  du_panel(app, cr, pad - 6, map_top - 6, W - 2 * pad + 12, map_h + 12, 0.36,
             app.du_map_opacity_pct);
  du.map_rect[0] = pad;
  du.map_rect[1] = map_top;
  du.map_rect[2] = W - 2 * pad;
  du.map_rect[3] = map_h;
  {
    cairo_save(cr);
    cairo_rectangle(cr, pad, map_top, W - 2 * pad, map_h);
    cairo_clip(cr);
    // The map follows window size: re-lay when the content rect moved,
    // throttled so resize drags don't re-index huge trees per tick.
    int mcx = pad + 6, mcy = map_top + 6;
    int mcw = W - 2 * pad - 12, mch = map_h - 12;
    if (mcw > 0 && mch > 0 &&
        (du.laid_map[0] != mcx || du.laid_map[1] != mcy ||
         du.laid_map[2] != mcw || du.laid_map[3] != mch)) {
      // Cheap: the children index is cached, this replays only squarify.
      du_layout_map(app, mcx, mcy, mcw, mch);
    }
    // Dim only when the highlight actually matches something in view —
    // a stale highlight must never wash the whole map.
    bool hl_live = du_highlight_live(app);
    // Framed-folder pill labels, collected here and painted deduped
    // after the loop (nested pills stack at the same corner otherwise).
    struct DuPill {
      double x = 0, y = 0, w = 0, h = 0;
      std::string name;
      std::string sub;
      double alpha = 1.0;
    };
    std::vector<DuPill> pills;
    // One hit for the whole map; tiles resolve by deepest-first geometry
    // search in the click handler (per-tile IDs can't survive a 3000 cap
    // in a 10-bit control field).
    app.hit_diskusage.add(du_hit(hui::Hit::kDuMapClick), pad, map_top,
                          W - 2 * pad, map_h);
    if (!du.tiles.empty()) {
      size_t ti = 0;
      for (auto& t : du.tiles) {
        bool dim = false;
        if (hl_live && !t.is_dir) {
          dim = true;
          for (auto& e : du.exts) {
            if (e.ext == du.highlight_ext &&
                std::abs(e.r - t.r) < 0.01 && std::abs(e.g - t.g) < 0.01 &&
                std::abs(e.b - t.b) < 0.01) {
              dim = false;
              break;
            }
          }
        }
        double alpha = dim ? 0.18 : 1.0;
        // 2px gutters (1px inset per side) so tile edges read at a glance.
        // Sub-pixel slivers still paint: at real scale they are the color.
        double ix = t.x + 1, iy = t.y + 1;
        double iw = t.w - 2, ih = t.h - 2;
        // No minimum size: at real scale sub-pixel files are the color.
        // (Degenerate rects can't happen: squarify only sees +sizes.)
        if (iw <= 0 || ih <= 0) continue;
        // Corner radius clamped: a fixed r=4 on a 1px sliver degenerates.
        double rr = std::min(4.0, std::min(iw, ih) / 2.0);
        // Basename + size line for labels below.
        std::string nm;
        {
          auto slash = t.path.rfind('/');
          nm = (slash == std::string::npos) ? t.path : t.path.substr(slash + 1);
        }
        std::string sz = du_human(t.size);
        if (t.is_dir) {
          auto dit = du.dirs.find(t.path);
          if (dit != du.dirs.end()) {
            char cbuf[64];
            snprintf(cbuf, sizeof(cbuf), "%llu files",
                     (unsigned long long)dit->second.files);
            sz += std::string(" · ") + cbuf;
          }
        }
        // Cushion/gradients only pay off on tiles big enough to see them;
        // slivers get a flat fill (also much faster at 3000 tiles).
        bool detailed = (iw * ih > 2000.0);
        if (t.framed) {
          // Subdivided directory: no fill — the children's mosaic shows
          // through. Dark frame + pill label only.
          cairo_set_source_rgba(cr, 0, 0, 0, 0.55 * alpha);
          cairo_set_line_width(cr, 1.5);
          draw_rounded_rect(cr, ix + 0.5, iy + 0.5, iw - 1, ih - 1, 4);
          cairo_stroke(cr);
          // Pill labels only on roomy frames (collected; deduped draw
          // below). Smaller folders identify via hover readout.
          if (iw > 120 && ih > 56) {
            DuPill pill;
            pill.alpha = alpha;
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 11);
            pill.name = hui::design::clip_end(cr, nm, iw - 28);
            cairo_text_extents_t ne;
            cairo_text_extents(cr, pill.name.c_str(), &ne);
            bool two = ih > 48;
            double sw2 = 0;
            if (two) {
              cairo_set_font_size(cr, 10);
              pill.sub = hui::design::clip_end(cr, sz, iw - 28);
              cairo_text_extents_t se;
              cairo_text_extents(cr, pill.sub.c_str(), &se);
              sw2 = se.x_advance;
            }
            pill.w = std::min(iw - 8.0, std::max(ne.x_advance, sw2) + 16.0);
            pill.h = two ? 40.0 : 22.0;
            pill.x = ix + 4;
            pill.y = iy + 4;
            pills.push_back(std::move(pill));
          }
        } else {
        // Base fill + cushion: soft radial highlight, darkened rim.
        cairo_set_source_rgba(cr, t.r, t.g, t.b, alpha);
        draw_rounded_rect(cr, ix, iy, iw, ih, rr);
        cairo_fill(cr);
        if (detailed) {
          double cx = ix + iw / 2, cy = iy + ih / 2;
          double rad = std::sqrt((iw / 2) * (iw / 2) + (ih / 2) * (ih / 2));
          cairo_pattern_t* pat = cairo_pattern_create_radial(
              cx, cy - ih * 0.15, 1.0, cx, cy, rad > 1.0 ? rad : 1.0);
          cairo_pattern_add_color_stop_rgba(pat, 0, 1, 1, 1,
                                            0.14 * alpha);
          cairo_pattern_add_color_stop_rgba(pat, 0.55, 1, 1, 1, 0.0);
          cairo_pattern_add_color_stop_rgba(pat, 1, 0, 0, 0,
                                            0.22 * alpha);
          cairo_set_source(cr, pat);
          draw_rounded_rect(cr, ix, iy, iw, ih, rr);
          cairo_fill(cr);
          cairo_pattern_destroy(pat);
        }
        cairo_set_source_rgba(cr, 0, 0, 0, 0.35 * alpha);
        cairo_set_line_width(cr, 1.0);
        draw_rounded_rect(cr, ix + 0.5, iy + 0.5, iw - 1, ih - 1, rr);
        cairo_stroke(cr);
        // Labels: only above a size floor — cramming text into slivers
        // produces unreadable fragments ("35...es"); hover covers those.
        if (iw > 72 && ih > 26) {
          cairo_set_source_rgba(cr, 1, 1, 1, 0.85 * alpha + 0.1);
          cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                 CAIRO_FONT_WEIGHT_NORMAL);
          cairo_set_font_size(cr, 11);
          cairo_move_to(cr, ix + 6, iy + 16);
          cairo_show_text(
              cr, hui::design::clip_end(cr, nm, iw - 12).c_str());
          if (ih > 48) {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.65 * alpha + 0.1);
            cairo_set_font_size(cr, 10);
            cairo_move_to(cr, ix + 6, iy + 31);
            cairo_show_text(
                cr, hui::design::clip_end(cr, sz, iw - 12).c_str());
          }
        }
        }
        bool sel = (t.path == du.selected);
        if (sel || static_cast<int>(ti) == du.hover_tile) {
          cairo_set_source_rgba(cr, 1, 1, 1, sel ? 0.95 : 0.5);
          cairo_set_line_width(cr, sel ? 2.0 : 1.2);
          draw_rounded_rect(cr, ix, iy, iw, ih, rr);
          cairo_stroke(cr);
        }
        ++ti;
      }
      // Pills, shallow-first, skipping any that would overlap an
      // accepted one (nested corners); losers use hover readout.
      {
        struct R {
          double x, y, w, h;
        };
        std::vector<R> placed;
        for (auto& pill : pills) {
          bool clash = false;
          for (auto& r : placed) {
            if (pill.x < r.x + r.w + 2 && r.x < pill.x + pill.w + 2 &&
                pill.y < r.y + r.h + 2 && r.y < pill.y + pill.h + 2) {
              clash = true;
              break;
            }
          }
          if (clash) continue;
          placed.push_back({pill.x, pill.y, pill.w, pill.h});
          cairo_set_source_rgba(cr, 0, 0, 0, 0.55 * pill.alpha);
          draw_rounded_rect(cr, pill.x, pill.y, pill.w, pill.h, 6);
          cairo_fill(cr);
          cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                 CAIRO_FONT_WEIGHT_BOLD);
          cairo_set_font_size(cr, 11);
          cairo_set_source_rgba(cr, 1, 1, 1, 0.92 * pill.alpha + 0.05);
          cairo_move_to(cr, pill.x + 8, pill.y + 15);
          cairo_show_text(cr, pill.name.c_str());
          if (!pill.sub.empty()) {
            cairo_set_font_size(cr, 10);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.75 * pill.alpha + 0.05);
            cairo_move_to(cr, pill.x + 8, pill.y + 30);
            cairo_show_text(cr, pill.sub.c_str());
          }
        }
      }
      // Hover tracking over stored geometry, deepest tile wins.
      du.hover_tile = -1;
      for (size_t i = 0; i < du.tiles.size(); ++i) {
        auto& t = du.tiles[i];
        if (t.w <= 0 || t.h <= 0) continue;
        if (app.pointerX >= t.x + 0.5 && app.pointerX < t.x + t.w - 0.5 &&
            app.pointerY >= t.y + 0.5 && app.pointerY < t.y + t.h - 0.5) {
          du.hover_tile = static_cast<int>(i);
        }
      }
    } else if (!scanning) {
      cairo_set_source_rgba(cr, app.text_secondary_r,
                            app.text_secondary_g, app.text_secondary_b, 0.8);
      cairo_set_font_size(cr, 13);
      const char* msg = du.files.empty() ? "Nothing here yet."
                                         : "Zoom in for the treemap.";
      cairo_move_to(cr, pad + 16, map_top + 28);
      cairo_show_text(cr, msg);
    }
    cairo_restore(cr);
  }

  {
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, pad, H - 30.5);
    cairo_line_to(cr, W - pad, H - 30.5);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    char buf[512];
    // Hover readout first: thin slivers identify via status, not labels.
    if (du.hover_tile >= 0 &&
        du.hover_tile < static_cast<int>(du.tiles.size())) {
      auto& t = du.tiles[static_cast<size_t>(du.hover_tile)];
      auto slash = t.path.rfind('/');
      std::string nm = (slash == std::string::npos)
                           ? t.path
                           : t.path.substr(slash + 1);
      snprintf(buf, sizeof(buf), "%s · %s", nm.c_str(),
               du_human(t.size).c_str());
    } else if (scanning) {
      snprintf(buf, sizeof(buf), "Scanning… %llu files · %s",
               (unsigned long long)du.p_files.load(),
               du_human(du.p_bytes.load()).c_str());
    } else if (du.dup_scanning.load(std::memory_order_relaxed)) {
      snprintf(buf, sizeof(buf), "Hashing… %llu / %llu files",
               (unsigned long long)du.dup_p_done.load(),
               (unsigned long long)du.dup_p_total.load());
    } else if (du.dupact_running.load(std::memory_order_relaxed)) {
      snprintf(buf, sizeof(buf), "Trashing… %llu / %llu",
               (unsigned long long)du.dupact_done.load(),
               (unsigned long long)du.dupact_total.load());
    } else if (!du.status_msg.empty() &&
               steady_ms() < du.status_until_ms) {
      snprintf(buf, sizeof(buf), "%s", du.status_msg.c_str());
    } else {
      snprintf(buf, sizeof(buf), "%llu files · %llu folders · %s total",
               (unsigned long long)du.total_files,
               (unsigned long long)du.total_dirs,
               du_human(du.total_bytes).c_str());
    }
    cairo_move_to(cr, pad, H - 12);
    cairo_show_text(cr, buf);
    // Right side: unreadable dirs (permission denied) and skipped virtual
    // filesystems. Inline reasons — this window has no tooltip layer.
    if (!scanning && (du.unknown_dirs > 0 || du.pruned_dirs > 0)) {
      char ebuf[160];
      if (du.unknown_dirs > 0 && du.pruned_dirs > 0) {
        snprintf(ebuf, sizeof(ebuf),
                 "%llu unreadable (permission denied) · %llu skipped (/proc /sys /dev)",
                 (unsigned long long)du.unknown_dirs,
                 (unsigned long long)du.pruned_dirs);
      } else if (du.unknown_dirs > 0) {
        snprintf(ebuf, sizeof(ebuf), "%llu unreadable (permission denied)",
                 (unsigned long long)du.unknown_dirs);
      } else {
        snprintf(ebuf, sizeof(ebuf), "%llu skipped (/proc /sys /dev)",
                 (unsigned long long)du.pruned_dirs);
      }
      cairo_text_extents_t ee;
      cairo_text_extents(cr, ebuf, &ee);
      cairo_move_to(cr, W - pad - ee.x_advance, H - 12);
      cairo_show_text(cr, ebuf);
    }
  }

  if (du.settings_open) {
    const int pw = 300;
    const int px = W - pad - pw;
    const int py = 90;
    const int ph = H - 40 - py;
    // Background first: swallows clicks over content beneath.
    app.hit_diskusage.add(du_hit(hui::Hit::kDuPanelSwallow), px, py, pw, ph);
    hui::design::card_fill(cr, app, 0.97);
    draw_rounded_rect(cr, px, py, pw, ph, 12);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_set_line_width(cr, 1.0);
    draw_rounded_rect(cr, px + 0.5, py + 0.5, pw - 1, ph - 1, 12);
    cairo_stroke(cr);
    // Tab chrome (Appearance today; room for more tabs later).
    {
      const int tx = px + 12, ty = py + 10, tw = 120, th = 28;
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                            0.28);
      draw_rounded_rect(cr, tx, ty, tw, th, 8);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 12);
      cairo_text_extents_t te;
      cairo_text_extents(cr, "Appearance", &te);
      cairo_move_to(cr, tx + (tw - te.x_advance) / 2, ty + 19);
      cairo_show_text(cr, "Appearance");
    }
    // × closes (same as the gear toggle).
    app.hit_diskusage.add(du_hit(hui::Hit::kDuSettings), px + pw - 34, py + 8,
                          26, 26);
    {
      bool hov =
          (app.pointerX >= px + pw - 34 && app.pointerX < px + pw - 8 &&
           app.pointerY >= py + 8 && app.pointerY < py + 34);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                            hov ? 1.0 : 0.6);
      cairo_set_font_size(cr, 15);
      cairo_move_to(cr, px + pw - 27, py + 26);
      cairo_show_text(cr, "×");
    }
    // Five opacity sliders: label + value + track/knob.
    const char* labels[5] = {"Drives", "Directory list", "Extension list",
                             "Treemap", "Background"};
    const int pcts[5] = {app.du_drives_opacity_pct, app.du_dir_opacity_pct,
                         app.du_ext_opacity_pct, app.du_map_opacity_pct,
                         app.du_bg_opacity_pct};
    const int sw = pw - 48;
    int ry = py + 56;
    for (int i = 0; i < 5; ++i) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 13);
      cairo_move_to(cr, px + 16, ry + 14);
      cairo_show_text(cr, labels[i]);
      char vbuf[16];
      snprintf(vbuf, sizeof(vbuf), "%d%%", pcts[i]);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 1.0);
      cairo_text_extents_t ve;
      cairo_text_extents(cr, vbuf, &ve);
      cairo_move_to(cr, px + pw - 16 - ve.x_advance, ry + 14);
      cairo_show_text(cr, vbuf);
      const int sx = px + 16, sy = ry + 26, sh = 6;
      du.slider_rect[i][0] = sx;
      du.slider_rect[i][1] = sy - 10;
      du.slider_rect[i][2] = sw;
      du.slider_rect[i][3] = sh + 20;
      app.hit_diskusage.add(du_hit(hui::Hit::kDuSliderBase + i), sx, sy - 10,
                            sw, sh + 20);
      double frac = std::clamp(pcts[i], 0, 100) / 100.0;
      cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
      draw_rounded_rect(cr, sx, sy, sw, sh, sh / 2);
      cairo_fill(cr);
      if (frac > 0.001) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                              0.9);
        draw_rounded_rect(cr, sx, sy, std::max(2.0, sw * frac), sh, sh / 2);
        cairo_fill(cr);
      }
      cairo_new_path(cr);
      cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
      cairo_arc(cr, sx + sw * frac, sy + sh / 2.0, 7.0, 0, 2 * M_PI);
      cairo_fill(cr);
      if (i == du.slider_drag - hui::Hit::kDuSliderBase) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                              0.9);
        cairo_set_line_width(cr, 2.0);
        cairo_arc(cr, sx + sw * frac, sy + sh / 2.0, 9.0, 0, 2 * M_PI);
        cairo_stroke(cr);
      }
      ry += 56;
    }
    // Color-engine note.
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 0.9);
    cairo_set_font_size(cr, 11);
    cairo_move_to(cr, px + 16, py + ph - 14);
    cairo_show_text(
        cr,
        hui::design::clip_end(cr, "Panels follow the color engine.", pw - 32)
            .c_str());
  }
}


namespace {

bool du_path_is_dir(AppState& app, const std::string& path) {
  auto& du = app.diskusage;
  if (du.dirs.find(path) != du.dirs.end()) return true;
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void du_drill(AppState& app, const std::string& dir) {
  auto& du = app.diskusage;
  du.cur_dir = dir;
  du.treemap_root = dir;
  du.selected.clear();
  du.scroll_dir = 0;
  du_rebuild_kids(app);
  du_refresh_views(app);
  app.du_pendingRedraw = true;
}

bool du_click_track(AppState& app, const std::string& path) {
  // Double-click detector (500 ms, same path). Returns true on double.
  auto& du = app.diskusage;
  int64_t now = steady_ms();
  bool dbl = (path == du.last_click_path && now - du.last_click_ms < 500);
  du.last_click_ms = now;
  du.last_click_path = path;
  return dbl;
}

void du_open_path(AppState& app, const std::string& path) {
  if (du_path_is_dir(app, path)) {
    du_drill(app, path);
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    execlp("xdg-open", "xdg-open", path.c_str(), nullptr);
    _exit(127);
  }
}

} // namespace

void handle_du_click(AppState& app, int x, int y, int button) {
  if (button != 0x110) return;
  auto& du = app.diskusage;
  uint32_t hid = app.hit_diskusage.query(x, y);
  if ((hid & hui::Hit::kGroupMask) != hui::Hit::kDialog) return;
  if ((hid & 0xFFFFC00) !=
      hui::Hit::dialog(hui::Hit::kDlgDiskUsage, 0))
    return;
  int ctrl = hui::Hit::dialog_ctrl(hid);
  const bool scanning = du.scanning.load(std::memory_order_relaxed);
  // Group-trash confirm is two-step: any other action disarms it.
  if (ctrl != hui::Hit::kDuDupTrashAll) du.dup_confirm_armed = false;

  if (ctrl == hui::Hit::kDuClose) {
    close_disk_usage(app);
    return;
  }
  if (ctrl == hui::Hit::kDuRescan) {
    if (scanning) return;
    du.cur_dir = du.root;
    du.treemap_root = du.root;
    du.selected.clear();
    du.highlight_ext.clear();
    du.scroll_dir = du.scroll_ext = 0;
    du_start_scan(app);
    app.du_pendingRedraw = true;
    return;
  }
  if (ctrl == hui::Hit::kDuStop) {
    if (scanning) {
      du_stop_scan(app);
      app.du_pendingRedraw = true;
    }
    return;
  }
  if (ctrl == hui::Hit::kDuUp) {
    if (du.cur_dir != du.root && !du.cur_dir.empty()) {
      std::string up = du.cur_dir;
      while (up.size() > 1 && up.back() == '/') up.pop_back();
      auto slash = up.rfind('/');
      std::string parent =
          (slash == std::string::npos || slash == 0) ? "/" : up.substr(0, slash);
      if (parent.size() < du.root.size()) parent = du.root;
      du_drill(app, parent);
    }
    return;
  }
  if (ctrl == hui::Hit::kDuZoom) {
    if (!du.selected.empty() && du_path_is_dir(app, du.selected))
      du_drill(app, du.selected);
    return;
  }
  if (ctrl == hui::Hit::kDuOpenLoc) {
    if (!du.selected.empty()) {
      std::string target = du.selected;
      if (!du_path_is_dir(app, target)) {
        auto slash = target.rfind('/');
        target = (slash == std::string::npos || slash == 0)
                     ? "/"
                     : target.substr(0, slash);
      }
      navigate_to(app, target);
      draw(app);
    }
    return;
  }
  if (ctrl == hui::Hit::kDuDelete) {
    if (!du.selected.empty()) {
      std::string err;
      if (du_trash_selected(app, err)) {
        du_status(app, "Moved to Trash");
        // Model changed under the duplicate index: rehash survivors.
        if (du.dup_mode) du_start_dup_scan(app);
      } else {
        du_status(app, err.empty() ? "Delete failed" : err);
      }
      app.du_pendingRedraw = true;
    }
    return;
  }
  if (ctrl == hui::Hit::kDuSettings) {
    // Settings toggle: local panel (own state, own file — never the file
    // manager settings). Closing persists (cheap, idempotent).
    if (du.settings_open) du_end_slider_drag(app);
    du.settings_open = !du.settings_open;
    du.slider_drag = 0;
    app.du_pendingRedraw = true;
    return;
  }
  if (ctrl == hui::Hit::kDuDuplicates) {
    // Duplicates view toggle (same panel as the directory list).
    if (!du.done || scanning) return;
    du.dup_mode = !du.dup_mode;
    du.dup_scroll = 0;
    du.hover_row = -1;
    if (du.dup_mode &&
        (!du.dup_done || du.dup_data_gen != du.du_data_gen)) {
      du_start_dup_scan(app);
    } else {
      du_stop_dup_scan(app);
    }
    app.du_pendingRedraw = true;
    return;
  }
  if (ctrl == hui::Hit::kDuReport) {
    // CSV report: blocking save picker (same zenity pattern as the file
    // manager's folder picker), then a plain file write.
    if (!du.done || scanning) return;
    std::string base = du.root;
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    std::string name;
    if (base == "/") {
      name = "root";
    } else {
      auto slash = base.rfind('/');
      name = (slash == std::string::npos) ? base : base.substr(slash + 1);
      if (name.empty()) name = "drive";
    }
    const char* home = std::getenv("HOME");
    std::string suggest =
        std::string(home ? home : "/tmp") + "/disk-usage-" + name + ".csv";
    std::string cmd = "zenity --file-selection --save --confirm-overwrite "
                      "--filename=\"" +
                      suggest + "\" 2>/dev/null";
    std::string picked;
    {
      FILE* f = ::popen(cmd.c_str(), "r");
      if (!f) {
        du_status(app, "Save dialog unavailable");
        app.du_pendingRedraw = true;
        return;
      }
      char buf[4096];
      if (fgets(buf, sizeof(buf), f)) {
        picked = buf;
        while (!picked.empty() &&
               (picked.back() == '\n' || picked.back() == '\r'))
          picked.pop_back();
      }
      ::pclose(f);
    }
    if (picked.empty()) {
      du_status(app, "Report cancelled");
    } else {
      std::string csv = du_build_report_csv(app);
      std::ofstream out(picked, std::ios::binary | std::ios::trunc);
      if (out) {
        out << csv;
        out.close();
        du_status(app, out.good()
                             ? ("Report saved to " + picked)
                             : ("Write failed: " + picked));
      } else {
        du_status(app, "Write failed: " + picked);
      }
    }
    app.du_pendingRedraw = true;
    return;
  }
  if (ctrl == hui::Hit::kDuDupTrashAll) {
    if (du.dup_groups.empty() ||
        du.dup_scanning.load(std::memory_order_relaxed) ||
        du.dupact_running.load(std::memory_order_relaxed))
      return;
    if (!du.dup_confirm_armed) {
      size_t n = 0;
      for (auto& g : du.dup_groups) n += g.paths.size() - 1;
      char buf[160];
      snprintf(buf, sizeof(buf),
               "Trash %zu duplicates (first of each kept)? Click again", n);
      du_status(app, buf);
      du.dup_confirm_armed = true;
      app.du_pendingRedraw = true;
      return;
    }
    du.dup_confirm_armed = false;
    du_start_dup_trash(app);
    app.du_pendingRedraw = true;
    return;
  }
  if (ctrl >= hui::Hit::kDuSliderBase &&
      ctrl < hui::Hit::kDuSliderBase + 5) {
    // Panel slider press: grab the drag and apply immediately.
    du.slider_drag = ctrl;
    du_apply_slider_at(app, ctrl, x);
    app.du_pendingRedraw = true;
    return;
  }
  if (ctrl == hui::Hit::kDuPanelSwallow) {
    // Settings panel background: absorb the click.
    return;
  }
  if (ctrl == hui::Hit::kDuDirScroll || ctrl == hui::Hit::kDuExtScroll) {
    // List scrollbar press: jump + grab the drag.
    du.scroll_drag = (ctrl == hui::Hit::kDuDirScroll) ? 1 : 2;
    du_apply_scroll_drag(app, y);
    app.du_pendingRedraw = true;
    return;
  }
  if (ctrl >= hui::Hit::kDuCrumbBase &&
      ctrl < hui::Hit::kDuCrumbBase + 24) {
    // Toolbar breadcrumb: drill straight to the segment.
    size_t i = static_cast<size_t>(ctrl - hui::Hit::kDuCrumbBase);
    if (i < du.crumb_paths.size() && !du.crumb_paths[i].empty()) {
      du_drill(app, du.crumb_paths[i]);
    }
    return;
  }
  if (ctrl >= hui::Hit::kDuDriveRowBase &&
      ctrl < hui::Hit::kDuDriveRowBase + 16) {
    int idx = ctrl - hui::Hit::kDuDriveRowBase;
    std::vector<std::string> local_roots;
    for (auto& loc : app.sidebar_locations) {
      // Same filter and order as the drives paint loop above.
      if (loc.kind != SidebarLocation::Kind::Drive &&
          loc.kind != SidebarLocation::Kind::Root)
        continue;
      if (loc.path.empty() || loc.path[0] != '/') continue;
      local_roots.push_back(loc.path);
    }
    if (idx >= 0 && idx < static_cast<int>(local_roots.size())) {
      du.root = local_roots[idx];
      du.cur_dir = du.root;
      du.treemap_root = du.root;
      du.selected.clear();
      du.highlight_ext.clear();
      du.scroll_dir = du.scroll_ext = 0;
      du_start_scan(app);
      app.du_pendingRedraw = true;
    }
    return;
  }
  if (ctrl >= hui::Hit::kDuDirRowBase &&
      ctrl < hui::Hit::kDuDirRowBase + 128) {
    // Same panel, two views. The ext-row id range overlaps this one, so
    // both branches verify the point is inside their own panel (a latent
    // misroute otherwise: ext clicks landing here silently die or worse).
    auto in_dir = [&] {
      return x >= du.dir_rect[0] && x < du.dir_rect[0] + du.dir_rect[2] &&
             y >= du.dir_rect[1] && y < du.dir_rect[1] + du.dir_rect[3];
    };
    if (!in_dir()) return;
    size_t i = static_cast<size_t>(ctrl - hui::Hit::kDuDirRowBase);
    if (du.dup_mode) {
      if (i >= du.dup_rows.size()) return;
      const auto& r = du.dup_rows[i];
      if (r.group >= du.dup_groups.size()) return;
      const auto& g = du.dup_groups[r.group];
      std::string path;
      if (r.header) {
        if (!g.paths.empty()) path = g.paths[0];
      } else if (r.member < g.paths.size()) {
        path = g.paths[r.member];
      }
      if (path.empty()) return;
      bool dbl = du_click_track(app, path);
      du.selected = path;
      if (dbl) du_open_path(app, path);
      app.du_pendingRedraw = true;
      return;
    }
    if (i >= du.dir_kids.size()) return;
    const auto& kid = du.dir_kids[i];
    bool dbl = du_click_track(app, kid.path);
    // Folders drill on single click (core WinDirStat interaction); files
    // select, double-click opens externally. Dirs stay selectable (for
    // Zoom/Delete) via the treemap.
    if (kid.is_dir) {
      du_drill(app, kid.path);
      return;
    }
    du.selected = kid.path;
    if (dbl) du_open_path(app, kid.path);
    app.du_pendingRedraw = true;
    return;
  }
  if (ctrl >= hui::Hit::kDuExtRowBase &&
      ctrl < hui::Hit::kDuExtRowBase + 64) {
    // Panel guard: this id range overlaps the directory rows above.
    if (x < du.ext_rect[0] || x >= du.ext_rect[0] + du.ext_rect[2] ||
        y < du.ext_rect[1] || y >= du.ext_rect[1] + du.ext_rect[3])
      return;
    size_t i = static_cast<size_t>(ctrl - hui::Hit::kDuExtRowBase);
    if (i >= du.exts.size()) return;
    const std::string& ext = du.exts[i].ext;
    du.highlight_ext = (du.highlight_ext == ext) ? "" : ext;
    app.du_pendingRedraw = true;
    return;
  }
  if (ctrl == hui::Hit::kDuMapClick) {
    // Deepest tile wins (children are laid out after parents).
    int best = -1;
    for (size_t i = 0; i < du.tiles.size(); ++i) {
      auto& t = du.tiles[i];
      if (t.w <= 0 || t.h <= 0) continue;
      if (x >= t.x + 0.5 && x < t.x + t.w - 0.5 && y >= t.y + 0.5 &&
          y < t.y + t.h - 0.5)
        best = static_cast<int>(i);
    }
    if (best < 0) return;
    const std::string& path = du.tiles[static_cast<size_t>(best)].path;
    const bool is_dir = du.tiles[static_cast<size_t>(best)].is_dir;
    // The rolled-up "<smaller>" bucket is not a real path.
    if (path.size() >= 9 &&
        path.compare(path.size() - 9, 9, "<smaller>") == 0)
      return;
    bool dbl = du_click_track(app, path);
    du.selected = path;
    // Files open externally on double-click; dirs drill on double-click
    // (single-click selects). The list drills dirs on single click.
    if (dbl) du_open_path(app, path);
    (void)is_dir;
    app.du_pendingRedraw = true;
    return;
  }
}

void du_apply_slider_at(AppState& app, int ctrl, int x) {
  auto& du = app.diskusage;
  int i = ctrl - hui::Hit::kDuSliderBase;
  if (i < 0 || i >= 5) return;
  const int* r = du.slider_rect[i];
  if (r[2] <= 0) return;
  double frac = static_cast<double>(x - r[0]) / static_cast<double>(r[2]);
  int val = std::clamp(static_cast<int>(frac * 100.0), 0, 100);
  if (i == 0)
    app.du_drives_opacity_pct = val;
  else if (i == 1)
    app.du_dir_opacity_pct = val;
  else if (i == 2)
    app.du_ext_opacity_pct = val;
  else if (i == 3)
    app.du_map_opacity_pct = val;
  else
    app.du_bg_opacity_pct = val;
  app.du_pendingRedraw = true;
}

void du_end_slider_drag(AppState& app) {
  auto& du = app.diskusage;
  du.slider_drag = 0;
  eh::config::DiskUsageSettings s;
  s.drives_opacity_pct = app.du_drives_opacity_pct;
  s.dir_opacity_pct = app.du_dir_opacity_pct;
  s.ext_opacity_pct = app.du_ext_opacity_pct;
  s.map_opacity_pct = app.du_map_opacity_pct;
  s.bg_opacity_pct = app.du_bg_opacity_pct;
  (void)eh::config::write_disk_usage_toml(s);
}

void handle_du_axis(AppState& app, int x, int y, double dy) {
  auto& du = app.diskusage;
  int step = static_cast<int>(dy > 0 ? 60 : -60);
  if (x >= du.dir_rect[0] && x < du.dir_rect[0] + du.dir_rect[2] &&
      y >= du.dir_rect[1] && y < du.dir_rect[1] + du.dir_rect[3]) {
    // Same panel scrolls the duplicates view in dup mode.
    if (du.dup_mode) {
      int max = std::max(0, du.dup_content_h - du.dir_rect[3]);
      du.dup_scroll = std::clamp(du.dup_scroll + step, 0, max);
    } else {
      int max = std::max(0, du.dir_content_h - du.dir_rect[3]);
      du.scroll_dir = std::clamp(du.scroll_dir + step, 0, max);
    }
    app.du_pendingRedraw = true;
  } else if (x >= du.ext_rect[0] && x < du.ext_rect[0] + du.ext_rect[2] &&
             y >= du.ext_rect[1] && y < du.ext_rect[1] + du.ext_rect[3]) {
    int max = std::max(0, du.ext_content_h - du.ext_rect[3]);
    du.scroll_ext = std::clamp(du.scroll_ext + step, 0, max);
    app.du_pendingRedraw = true;
  }
}

bool handle_du_key(AppState& app, uint32_t sym, bool ctrl) {
  auto& du = app.diskusage;
  // Copy selected path (Baobab parity; headless-safe: copy_text no-ops
  // without a Wayland seat).
  if (ctrl && (sym == XKB_KEY_c || sym == XKB_KEY_C)) {
    if (!du.selected.empty()) {
      if (app.clipboard.copy_text(du.selected))
        du_status(app, "Path copied");
      else
        du_status(app, "Copy unavailable");
      app.du_pendingRedraw = true;
    }
    return true;
  }
  if (sym == XKB_KEY_Escape) {
    // Panel first (persisting), then the window.
    if (app.diskusage.settings_open) {
      du_end_slider_drag(app);
      app.diskusage.settings_open = false;
      app.diskusage.slider_drag = 0;
      app.du_pendingRedraw = true;
      return true;
    }
    close_disk_usage(app);
    return true;
  }
  if (sym == XKB_KEY_Delete || sym == XKB_KEY_KP_Delete) {
    auto& du = app.diskusage;
    if (!du.selected.empty()) {
      std::string err;
      if (du_trash_selected(app, err)) {
        du_status(app, "Moved to Trash");
        schedule_trash_maintain(app);
        if (du.dup_mode) du_start_dup_scan(app);
      } else {
        du_status(app, err.empty() ? "Delete failed" : err);
      }
      app.du_pendingRedraw = true;
    }
    return true;
  }
  if (sym == XKB_KEY_F5 || sym == XKB_KEY_r || sym == XKB_KEY_R) {
    if (!app.diskusage.scanning.load(std::memory_order_relaxed)) {
      app.diskusage.cur_dir = app.diskusage.root;
      app.diskusage.treemap_root = app.diskusage.root;
      app.diskusage.selected.clear();
      du_start_scan(app);
      app.du_pendingRedraw = true;
    }
    return true;
  }
  return false;
}

} // namespace eh::file_browser
