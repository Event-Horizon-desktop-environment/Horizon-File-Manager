#include "app/file_browser/app.hpp"
#include "app/file_browser/features/sidebar/sidebar.hpp"
#include "app/file_browser/features/selection/selection.hpp"
#include "app/file_browser/trace.hpp"

#include <cairo/cairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include <unistd.h>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>

#include "config/shell_config.hpp"
#include "platform/common/palette/matugen_palette.hpp"
#include "theme/core/primitives/box.hpp"

namespace eh::file_browser {

// ── paint (extracted draw logic, no buffer/surface dependency) ────

namespace {
struct PaintPhase {
  const char* name;
  std::chrono::steady_clock::time_point t0;
  std::vector<std::pair<const char*, double>>* log;
  std::vector<std::pair<const char*, double>>* sink; // resize-trace sink
  bool capture_all; // bench profiling: record every phase, no >=1 ms gate
  ~PaintPhase() {
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
    // Always record the first paint's phases (startup diagnostics); after
    // that only >=50 ms phases are worth logging.
    static std::atomic<int> first_paint{0};
    bool first = first_paint.fetch_add(1, std::memory_order_relaxed) < 40;
    if (ms >= 50.0 || (first && ms >= 0.5)) log->emplace_back(name, ms);
    if (sink && (capture_all || ms >= 1.0)) sink->emplace_back(name, ms);
  }
};
}  // namespace

// ── content scroll-delta reuse (partial repaint) ─────────────────

// Content column geometry (bench identity probe). The real geometry lives
// in features/sidebar.cpp (sidebar_content_geometry) so paint(), the reuse
// gate and the painter all share one fold-aware source of truth.
void content_reuse_geometry(AppState& app, int& cx, int& cy, int& cw, int& ch,
                            int& banner_h) {
  sidebar_content_geometry(app, cx, cy, cw, ch, banner_h);
}

// Shift the content clip rect [cx..cx+cw) x [cy..cy+ch) of `cr`'s target
// (an ARGB32 image surface) vertically by `delta` rows, in place. Positive
// delta = content moves up (new content appears at the bottom). Rows outside
// the copied range keep their old bytes; the caller must refill the newly
// exposed band. Returned rows copy src→dst in an order that guarantees no
// write clobbers a not-yet-read source row (verified: +delta ascending,
// -delta descending).
static void shift_content_rows(cairo_t* cr, int cx, int cy, int cw, int ch,
                               int delta) {
  if (delta == 0) return;
  cairo_surface_t* s = cairo_get_target(cr);
  if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) return;
  if (cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE) return;
  if (cairo_image_surface_get_format(s) != CAIRO_FORMAT_ARGB32) return;
  cairo_surface_flush(s);
  uint32_t* data = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(s));
  if (!data) return;
  const int stride = cairo_image_surface_get_stride(s) / 4;
  const int ad = std::abs(delta);
  const int copy = ch - ad;
  if (copy <= 0 || cw <= 0) { cairo_surface_mark_dirty_rectangle(s, cx, cy, cw, ch); return; }
  const uint32_t* src = data + (delta > 0 ? cy + delta : cy) * stride;
  uint32_t* dst = data + (delta > 0 ? cy : cy + ad) * stride;
  if (delta > 0) {          // shift up: write low rows first
    for (int i = 0; i < copy; ++i)
      memcpy(dst + i * stride + cx, src + i * stride + cx,
             static_cast<size_t>(cw) * 4u);
  } else {                  // shift down: write high rows first
    for (int i = copy - 1; i >= 0; --i)
      memcpy(dst + i * stride + cx, src + i * stride + cx,
             static_cast<size_t>(cw) * 4u);
  }
  cairo_surface_mark_dirty_rectangle(s, cx, cy, cw, ch);
}

// Signature of everything that affects content-column pixels except the
// scroll offset. A match means "the only possible change is scroll".
static uint64_t content_reuse_key(AppState& app, int cx, int cy, int cw,
                                  int ch) {
  auto& t = app.cur_tab();
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
  const auto q8 = [](double v) { return static_cast<uint64_t>(v * 2048.0); };
  mix(static_cast<uint64_t>(t.view_mode));
  mix(static_cast<uint64_t>(std::lround(app.zoom_pct)));
  mix(static_cast<uint64_t>(cx));
  mix(static_cast<uint64_t>(cy));
  mix(static_cast<uint64_t>(cw));
  mix(static_cast<uint64_t>(ch));
  mix(static_cast<uint64_t>(app.surface_opacity_pct));
  mix(q8(app.bg_r));
  mix(q8(app.bg_g));
  mix(q8(app.bg_b));
  mix(q8(app.surface_r));
  mix(q8(app.surface_g));
  mix(q8(app.surface_b));
  mix(q8(app.text_r));
  mix(q8(app.text_g));
  mix(q8(app.text_b));
  mix(q8(app.accent_r));
  mix(q8(app.accent_g));
  mix(q8(app.accent_b));
  mix(static_cast<uint64_t>(t.hover_idx));
  mix(static_cast<uint64_t>(t.selected_idx));
  mix(static_cast<uint64_t>(t.multi_selected.size()));
  for (int v : t.multi_selected) mix(static_cast<uint64_t>(v));
  mix(app.listing_epoch);
  mix(static_cast<uint64_t>(t.entries.size()));
  mix(static_cast<uint64_t>(t.visible_entries.size()));
  mix(static_cast<uint64_t>(std::hash<std::string>{}(t.current_path)));
  mix(static_cast<uint64_t>(t.dir_mtime));
  mix(static_cast<uint64_t>(t.group_field));
  mix(static_cast<uint64_t>(t.sort_field));
  mix(t.sort_descending ? 1u : 0u);
  mix(app.show_hidden ? 1u : 0u);
  mix(!app.drop_target_path.empty() ? 1u : 0u);
  mix(static_cast<uint64_t>(app.drop_target_idx));
  mix(app.drop_target_is_sidebar ? 1u : 0u);
  mix(static_cast<uint64_t>(app.cut_paths.size()));
  return h;
}

ContentReuseHint make_content_reuse_hint(AppState& app) {
  ContentReuseHint h;
  int cx2, cy2, cw2, ch2, bh2;
  sidebar_content_geometry(app, cx2, cy2, cw2, ch2, bh2);
  const bool maybe_reuse =
      !app.split_view && app.cur_tab().view_mode == ViewMode::Grid &&
      !app.marquee_active && !app.context_menu_open && !app.preview_active &&
      !app.startup_blank_frame;
  if (maybe_reuse && app.content_reuse.valid &&
      app.content_reuse.last_bi >= 0 && app.content_reuse.last_bi < 2 &&
      app.content_reuse.last_key == content_reuse_key(app, cx2, cy2, cw2, ch2) &&
      cw2 > 0 && ch2 > 0) {
    int delta = app.cur_tab().scroll_px - app.content_reuse.last_scroll;
    if (delta != 0 && std::abs(delta) < ch2 &&
        !app.buf[app.content_reuse.last_bi].busy())
      h.delta = delta;
  }
  return h;
}

void record_content_reuse(AppState& app, int buffer_index) {
  int cx2, cy2, cw2, ch2, bh2;
  sidebar_content_geometry(app, cx2, cy2, cw2, ch2, bh2);
  app.content_reuse.last_bi = buffer_index;
  app.content_reuse.last_scroll = app.cur_tab().scroll_px;
  app.content_reuse.last_key = content_reuse_key(app, cx2, cy2, cw2, ch2);
  app.content_reuse.valid =
      !app.split_view && app.cur_tab().view_mode == ViewMode::Grid &&
      !app.marquee_active && !app.context_menu_open && !app.preview_active &&
      !app.startup_blank_frame;
}

// Advance smooth scroll -> scroll_px for the frame being built. Called by
// draw()/bench BEFORE the reuse gate (so the gate sees the scroll it will
// render) and by paint() itself when no ContentReuseHint is supplied (raw
// full paints). Keeping it split from paint() lets the reuse decision read
// the freshly-advanced scroll_px.
void advance_scroll_render(AppState& app) {
  auto smooth_scroll_tab = [&](Tab& tab) {
    bool scrolling = std::abs(tab.scroll_smooth_current - tab.scroll_smooth_target) > 0.5;
    if (scrolling) {
      timespec ts{};
      clock_gettime(CLOCK_MONOTONIC, &ts);
      auto now_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                    static_cast<uint64_t>(ts.tv_nsec);
      double elapsed_ms =
          static_cast<double>(now_ns - app.scroll_anim_start_ns) / 1000000.0;
      constexpr double kTau = 40.0;
      double factor = 1.0 - std::exp(-elapsed_ms / kTau);
      double diff = tab.scroll_smooth_target - tab.scroll_smooth_current;
      tab.scroll_smooth_current += diff * factor;
      if (std::abs(tab.scroll_smooth_current - tab.scroll_smooth_target) <= 0.5) {
        tab.scroll_smooth_current = tab.scroll_smooth_target;
      }
      tab.scroll_px = static_cast<int>(std::lround(tab.scroll_smooth_current));
      app.scroll_needs_redraw = true;
    } else {
      tab.scroll_px = static_cast<int>(std::lround(tab.scroll_smooth_target));
      tab.scroll_smooth_current = tab.scroll_smooth_target;
    }
  };

  smooth_scroll_tab(app.cur_tab());
  if (app.split_view) smooth_scroll_tab(app.right_pane);

  // Smooth scroll (computer view)
  bool comp_scrolling = std::abs(app.computer_scroll_smooth_current - app.computer_scroll_smooth_target) > 0.5;
  if (comp_scrolling) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    auto now_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                  static_cast<uint64_t>(ts.tv_nsec);
    double elapsed_ms =
        static_cast<double>(now_ns - app.scroll_anim_start_ns) / 1000000.0;
    constexpr double kTau = 40.0;
    double factor = 1.0 - std::exp(-elapsed_ms / kTau);
    double diff = app.computer_scroll_smooth_target - app.computer_scroll_smooth_current;
    app.computer_scroll_smooth_current += diff * factor;
    if (std::abs(app.computer_scroll_smooth_current - app.computer_scroll_smooth_target) <= 0.5) {
      app.computer_scroll_smooth_current = app.computer_scroll_smooth_target;
    }
    app.computer_scroll_px = static_cast<int>(std::lround(app.computer_scroll_smooth_current));
    app.scroll_needs_redraw = true;
  } else {
    app.computer_scroll_px = static_cast<int>(std::lround(app.computer_scroll_smooth_target));
    app.computer_scroll_smooth_current = app.computer_scroll_smooth_target;
  }

  bool tab_scrolling = std::abs(app.cur_tab().scroll_smooth_current - app.cur_tab().scroll_smooth_target) > 0.5;
  if (app.split_view)
    tab_scrolling = tab_scrolling ||
      std::abs(app.right_pane.scroll_smooth_current - app.right_pane.scroll_smooth_target) > 0.5;

  if (!tab_scrolling && !comp_scrolling) {
    app.scroll_needs_redraw = false;
  }
}

// ── main draw ────────────────────────────────────────────────────

void paint(AppState& app, cairo_t* cr, ContentReuseHint* reuse) {
  std::vector<std::pair<const char*, double>> slow_phases;
  auto phase = [&app, &slow_phases](const char* n) {
    return PaintPhase{n, std::chrono::steady_clock::now(), &slow_phases,
                      (app.resize_session_active &&
                       eh::trace::enabled().load(std::memory_order_relaxed))
                          ? &app.resize_phase_samples
                          : nullptr,
                      app.paint_profile};
  };
  // Check if settings TOMLs changed and re-apply settings if so
  {
    static timespec s_last_settings_mtime{};
    static bool s_initialized_settings = false;
    const std::string toml_path = eh::config::state_settings_toml_path();
    struct stat st{};
    if (::stat(toml_path.c_str(), &st) == 0) {
      if (!s_initialized_settings) {
        s_last_settings_mtime = st.st_mtim;
        s_initialized_settings = true;
      } else if (st.st_mtim.tv_sec != s_last_settings_mtime.tv_sec ||
                 st.st_mtim.tv_nsec != s_last_settings_mtime.tv_nsec) {
        s_last_settings_mtime = st.st_mtim;
        reload_settings_from_config(app);
      }
    }
    static timespec s_last_fb_mtime{};
    static bool s_initialized_fb = false;
    const std::string fb_path = eh::config::state_file_browser_toml_path();
    struct stat fb_st{};
    if (::stat(fb_path.c_str(), &fb_st) == 0) {
      if (!s_initialized_fb) {
        s_last_fb_mtime = fb_st.st_mtim;
        s_initialized_fb = true;
      } else if (fb_st.st_mtim.tv_sec != s_last_fb_mtime.tv_sec ||
                 fb_st.st_mtim.tv_nsec != s_last_fb_mtime.tv_nsec) {
        s_last_fb_mtime = fb_st.st_mtim;
        reload_settings_from_config(app);
      }
    }
    static timespec s_last_shell_color_mtime{};
    static bool s_initialized_shell_color = false;
    const std::string sc_path = eh::matugen::shell_color_config_path();
    struct stat sc_st{};
    if (::stat(sc_path.c_str(), &sc_st) == 0) {
      if (!s_initialized_shell_color) {
        s_last_shell_color_mtime = sc_st.st_mtim;
        s_initialized_shell_color = true;
      } else if (sc_st.st_mtim.tv_sec != s_last_shell_color_mtime.tv_sec ||
                 sc_st.st_mtim.tv_nsec != s_last_shell_color_mtime.tv_nsec) {
        s_last_shell_color_mtime = sc_st.st_mtim;
        reload_settings_from_config(app);
      }
    }
  }

  // Reset thumbnail decode budget for this frame
  app.thumb_decodes_this_frame = 0;
  app.thumb_pending_queue.clear();

  // Rebuilt below by every draw site: input reads these rects instead of
  // re-deriving geometry (ui/hit_registry.hpp).
  app.hit_main.clear();

  // Scrollbar hit rects are rebuilt by draw_scrollbar() during this frame
  app.scrollbar_rects.clear();

  if (!reuse) advance_scroll_render(app);

  // ── Operations panel slide animation ──
  {
    double target = app.ops_panel_open ? 1.0 : 0.0;
    double diff = target - app.ops_panel_slide;
    if (std::abs(diff) > 0.01) {
      double factor = 0.12;
      app.ops_panel_slide += diff * factor;
      app.scroll_needs_redraw = true;
    } else {
      app.ops_panel_slide = target;
    }
  }

  // Close ops panel when operation finishes. op_progress can be null here:
  // fast operations complete before the UI assigns the shared pointer (the
  // completion callback resets it), which used to leave the panel stuck open.
  if (app.ops_panel_open &&
      (!app.op_progress || !app.op_progress->active.load())) {
    app.ops_panel_open = false;
  }

  int w = app.width;
  int h = app.height;
  int top_h = app.top_bar_height;
  app.tab_bar_height = (app.tabs.size() > 1)
      ? std::max(30, static_cast<int>(std::lround(44.0 * app.zoom_pct / 100.0)))
      : 0;
  int tab_h = app.tab_bar_height;
  int status_h = app.status_bar_height;

  // Adaptive sidebar fold (module owns the fold/flap state so the fold
  // condition and the geometry can never drift apart).
  update_sidebar_fold(app, w);

  // Fit sidebar width to longest label
  if (app.sidebar_expanded && !app.sidebar_locations.empty()) {
    cairo_save(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0 * 1.2);
    double max_text_w = 0;
    for (const auto& loc : app.sidebar_locations) {
      cairo_text_extents_t te;
      cairo_text_extents(cr, loc.label.c_str(), &te);
      if (te.x_advance > max_text_w) max_text_w = te.x_advance;
    }
    cairo_restore(cr);
    int needed = static_cast<int>(std::ceil(68.0 + max_text_w));
    int new_base = std::max(150, needed);
    if (new_base != app.sidebar_width_base) {
      app.sidebar_width_base = new_base;
      app.sidebar_width = std::max(120, static_cast<int>(app.sidebar_width_base * app.zoom_pct / 100.0));
    }
  }

  int info_panel_w = 0;
  if (app.info_panel_open) {
    app.info_panel_width = std::max(200, static_cast<int>(280 * app.zoom_pct / 100.0));
    info_panel_w = app.info_panel_width;
  }
  // Ops panel floats over the content as an overlay; keep its zoom-scaled
  // width updated but it never resizes the content column.
  if (app.ops_panel_slide > 0.01)
    app.ops_panel_width = std::max(240, static_cast<int>(320 * app.zoom_pct / 100.0));
  size_sidebar_to_content(app, cr);
  int sidebar_w = app.sidebar_w();
  // Info panel must never squeeze the content column to nothing.
  if (info_panel_w > 0) {
    int max_info = std::max(160, w - sidebar_w - 240);
    if (info_panel_w > max_info) {
      app.info_panel_width = max_info;
      info_panel_w = max_info;
    }
  }
  int content_x, content_y, content_w, view_h, banner_h;
  sidebar_content_geometry(app, content_x, content_y, content_w, view_h, banner_h);
  bool search_banner_on = (app.search_active || app.recursive_search_active ||
                           app.r_search_active || app.r_recursive_search_active);
  int selector_h = (app.select_dir_mode || app.select_file_mode) ? app.select_bar_h : 0;
  int pane_top_h = app.split_view ? app.top_bar_height : 0;

  // Update info panel metadata if selection changed
  if (app.info_panel_open) {
    std::string cur_path;
    int si = app.cur_tab().selected_idx;
    if (si >= 0 && si < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int ri = app.cur_tab().visible_entries[si];
      if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size()))
        cur_path = app.cur_tab().entries[ri].path;
    }
    if (cur_path != app.info_panel_path) {
      app.info_panel_path = cur_path;
      app.info_panel_name.clear();
      app.info_panel_size = 0;
      app.info_panel_modified_sec = 0;
      app.info_panel_is_dir = false;
      app.info_panel_owner.clear();
      app.info_panel_group.clear();
      app.info_panel_mode = 0;
      app.info_panel_mime_type.clear();

      if (!cur_path.empty()) {
        auto& e = app.cur_tab().entries[app.cur_tab().visible_entries[app.cur_tab().selected_idx]];
        app.info_panel_name = e.name;
        app.info_panel_is_dir = e.is_dir;
        struct stat st;
        if (stat(cur_path.c_str(), &st) == 0) {
          app.info_panel_size = static_cast<uint64_t>(st.st_size);
          app.info_panel_modified_sec = st.st_mtime;
          app.info_panel_mode = st.st_mode;
          struct passwd* pw = getpwuid(st.st_uid);
          app.info_panel_owner = pw ? pw->pw_name : std::to_string(st.st_uid);
          struct group* gr = getgrgid(st.st_gid);
          app.info_panel_group = gr ? gr->gr_name : std::to_string(st.st_gid);
        }
        if (!e.mime_type.empty())
          app.info_panel_mime_type = e.mime_type;
      }
    }
  }

  double surf_alpha = app.surface_opacity_pct / 100.0;

  const bool reuse_scroll =
      (reuse != nullptr && reuse->delta != 0) &&
      app.cur_tab().view_mode == ViewMode::Grid;
  // Content scroll-delta reuse: draw() skipped the whole-window CLEAR so the
  // retained content column can be shifted in place later. Every shell layer
  // that repaints this frame must therefore composite over the SAME base a
  // full frame used (transparent post-CLEAR, or the content-column bg fill
  // where that full fill covered chrome), or translucent chrome (sidebar
  // body, separator, scrollbar, top/tab/status bars) alpha-stacks onto its
  // own previous-frame pixels on every scroll step.
  if (reuse_scroll) {
    // Clear the chrome regions outside the reused content column.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr, 0, 0, w, content_y);
    cairo_rectangle(cr, 0, content_y + view_h, w, h);
    cairo_rectangle(cr, 0, content_y, content_x, view_h);
    cairo_rectangle(cr, content_x + content_w, content_y,
                    w - content_x - content_w, view_h);
    // Sidebar separator column (1px line + 2px neighbor): a full frame
    // forms each row of this strip from ONE bg alpha + the outline line
    // (bg fill, then paint_inline_sidebar's 0.3-alpha line). The reuse
    // frame must produce the same composition, so erase the previous
    // frame's line-over-bg pixels here and let the bg re-lay + sidebar
    // line repaint them exactly once — otherwise the 0.53 surf_alpha bg
    // stacks on top of the retained line pixels on every scroll step.
    cairo_rectangle(cr, content_x, 0, 3, h - status_h);
    cairo_fill(cr);
    cairo_restore(cr);

    // Re-lay the content-column bg under the chrome strips that full frames
    // paint over it (full's fill covered [sidebar_w, 0, content_w, h-status_h]
    // but the reuse path skips it to keep the interior for the shift).
    cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, surf_alpha);
    // Top strip (topbar/tabbar/search-banner rows over the content column).
    if (content_w > 3)
      cairo_rectangle(cr, content_x + 3, 0, content_w - 3, content_y);
    // Sidebar separator column (x=content_x..content_x+3, full column height;
    // 1px normal, 3px while the resize handle is hovered).
    cairo_rectangle(cr, content_x, 0, 3, h - status_h);
    // Bottom strip between the content column and the status bar. A full
    // frame has NO content-column bg under the status bar (the bg fill stops
    // at h-status_h, which is exactly content_y+view_h when no selector bar
    // is showing), so the status bar paints over transparency and re-laying
    // bg here would alpha-stack under it. Only the optional selector rows
    // [content_y+view_h, h-status_h) sit on the bg fill in a full frame.
    int selector_h = h - status_h - (content_y + view_h);
    if (selector_h > 0)
      cairo_rectangle(cr, content_x, content_y + view_h, content_w, selector_h);
    cairo_fill(cr);
  }

  // Background (content column). Skipped on scroll-delta reuse frames: the
  // previous frame's content pixels must survive until shifted in place.
  if (!(reuse && reuse->delta != 0)) {
    cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, surf_alpha);
    cairo_rectangle(cr, sidebar_w, 0, content_w, h - status_h);
    cairo_fill(cr);
  }

  // Sidebar (inline column + separator/resize handle; the module computes
  // its own fold-aware width so content geometry and painting stay in sync).
  paint_inline_sidebar(app, cr, h, status_h, view_h);

  // Content area
  cairo_save(cr);
  cairo_rectangle(cr, content_x, content_y, content_w, view_h);
  cairo_clip(cr);

  if (app.split_view) {
    int div_w = 4;
    int split = app.split_divider_x;
    if (split <= 0) split = content_w / 2;
    int left_w = std::max(100, split - div_w / 2);
    int right_x = std::min(content_x + content_w - 100, content_x + split + div_w / 2);
    int right_w = content_x + content_w - right_x;
    int list_y = content_y + pane_top_h;
    int list_h = view_h - pane_top_h;

    // Left pane
    app.active_pane = 0;
    {
      cairo_save(cr);
      cairo_translate(cr, 0, content_y);
      draw_top_bar(app, cr, left_w, pane_top_h, content_y, content_x, left_w);
      cairo_restore(cr);
      int sb_lx = right_x - 10;
      if (app.cur_tab().view_mode == ViewMode::List) {
        draw_scrollbar(app, cr, sb_lx, list_y, list_h, app.cur_tab().content_h, list_h,
                       app.cur_tab().scroll_px, app.outline_r, app.outline_g, app.outline_b);
        draw_list_view(app, cr, content_x, list_y, left_w, list_h);
      } else if (app.cur_tab().view_mode == ViewMode::Grid) {
        draw_scrollbar(app, cr, sb_lx, list_y, list_h, app.cur_tab().content_h, list_h,
                       app.cur_tab().scroll_px, app.outline_r, app.outline_g, app.outline_b);
        draw_grid_view(app, cr, content_x, list_y, left_w, list_h);
      } else if (app.cur_tab().view_mode == ViewMode::Computer) {
        draw_computer_view(app, cr, content_x, list_y, left_w, list_h);
      } else if (app.cur_tab().view_mode == ViewMode::Tree) {
        draw_scrollbar(app, cr, sb_lx, list_y, list_h, app.cur_tab().content_h, list_h,
                       app.cur_tab().scroll_px, app.outline_r, app.outline_g, app.outline_b);
        draw_tree_view(app, cr, content_x, list_y, left_w, list_h);
      } else if (app.cur_tab().view_mode == ViewMode::Compact) {
        draw_scrollbar(app, cr, sb_lx, list_y, list_h, app.cur_tab().content_h, list_h,
                       app.cur_tab().scroll_px, app.outline_r, app.outline_g, app.outline_b);
        draw_compact_view(app, cr, content_x, list_y, left_w, list_h);
      }
    }
    cairo_restore(cr);

    // Divider
    cairo_save(cr);
    int div_x = content_x + split;
    if (app.split_divider_hover || app.split_divider_dragging) {
      cairo_set_source_rgba(cr, 0.4, 0.6, 1.0, 0.6);
    } else {
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
    }
    cairo_rectangle(cr, div_x, content_y, div_w, view_h);
    cairo_fill(cr);
    cairo_restore(cr);

    // Right pane
    cairo_save(cr);
    cairo_rectangle(cr, right_x, content_y, right_w, view_h);
    cairo_clip(cr);
    app.active_pane = 1;
    {
      cairo_save(cr);
      cairo_translate(cr, 0, content_y);
      draw_top_bar(app, cr, right_w, pane_top_h, content_y, right_x, right_w);
      cairo_restore(cr);
      int sb_rx = content_x + content_w - 10;
      if (app.cur_tab().view_mode == ViewMode::List) {
        draw_scrollbar(app, cr, sb_rx, list_y, list_h, app.cur_tab().content_h, list_h,
                       app.cur_tab().scroll_px, app.outline_r, app.outline_g, app.outline_b);
        draw_list_view(app, cr, right_x, list_y, right_w, list_h);
      } else if (app.cur_tab().view_mode == ViewMode::Grid) {
        draw_scrollbar(app, cr, sb_rx, list_y, list_h, app.cur_tab().content_h, list_h,
                       app.cur_tab().scroll_px, app.outline_r, app.outline_g, app.outline_b);
        draw_grid_view(app, cr, right_x, list_y, right_w, list_h);
      } else if (app.cur_tab().view_mode == ViewMode::Computer) {
        draw_computer_view(app, cr, right_x, list_y, right_w, list_h);
      } else if (app.cur_tab().view_mode == ViewMode::Tree) {
        draw_scrollbar(app, cr, sb_rx, list_y, list_h, app.cur_tab().content_h, list_h,
                       app.cur_tab().scroll_px, app.outline_r, app.outline_g, app.outline_b);
        draw_tree_view(app, cr, right_x, list_y, right_w, list_h);
      } else if (app.cur_tab().view_mode == ViewMode::Compact) {
        draw_scrollbar(app, cr, sb_rx, list_y, list_h, app.cur_tab().content_h, list_h,
                       app.cur_tab().scroll_px, app.outline_r, app.outline_g, app.outline_b);
        draw_compact_view(app, cr, right_x, list_y, right_w, list_h);
      }
    }
    app.active_pane = 0;
} else {
    // Single pane
    int sb_x = w - info_panel_w - 10;
    if (app.cur_tab().view_mode == ViewMode::Computer) {
      draw_scrollbar(app, cr, sb_x, content_y, view_h, app.computer_content_h, view_h,
                     app.computer_scroll_px, app.outline_r, app.outline_g, app.outline_b, true);
    } else if (!reuse_scroll) {
      // On scrolling reuse frames the scrollbar is drawn only after the
      // content shift (it would otherwise be moved by the shift while the
      // in-place redraw re-blends it). Full frames draw it here as usual.
      draw_scrollbar(app, cr, sb_x, content_y, view_h, app.cur_tab().content_h, view_h,
                     app.cur_tab().scroll_px, app.outline_r, app.outline_g, app.outline_b);
    }
    // Content scroll-delta reuse: only the newly exposed band is repainted;
    // the rest of the content column is shifted in place from the previous
    // frame (the caller verified nothing else changed).
    if (reuse_scroll) {
      const int ad = std::abs(reuse->delta);
      int band_y0 = reuse->delta > 0 ? content_y + view_h - ad : content_y;
      int band_y1 = band_y0 + ad;
      if (ad < view_h) {
        {
          auto ph = phase("grid-scroll");
          shift_content_rows(cr, content_x, content_y, content_w, view_h,
                             reuse->delta);
          // The exposed band must be composited over the same base the full
          // path used (transparent, post-CLEAR) — simply re-filling bg over
          // the retained pre pixels stacks the surface alpha a second time
          // whenever surf_alpha < 1. Erase first, then fill + strip-draw.
          cairo_save(cr);
          // Skip the 1px sidebar separator column (x=content_x): the band rows
          // there already hold this frame's separator-over-bg composition
          // (nothing sampled that column), so wiping it would drop the line.
          cairo_rectangle(cr, content_x + 1, band_y0,
                          std::max(0, content_w - 1), band_y1 - band_y0);
          cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
          cairo_fill(cr);
          cairo_restore(cr);
          cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, surf_alpha);
          cairo_rectangle(cr, content_x + 1, band_y0,
                          std::max(0, content_w - 1), band_y1 - band_y0);
          cairo_fill(cr);
          draw_grid_view(app, cr, content_x, content_y, content_w, view_h,
                         band_y0, band_y1);
          // Scrollbar thumb moved with the content. It composites over the
          // content-column bg in a full frame (bg fill, then scrollbar), so
          // on a reuse frame its 6px strip must be erased and re-laid with
          // bg first — otherwise the retained pre-scroll thumb pixels
          // alpha-stack under the new thumb.
          cairo_save(cr);
          cairo_rectangle(cr, sb_x, content_y, 6, view_h);
          cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
          cairo_fill(cr);
          cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
          cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, surf_alpha);
          cairo_rectangle(cr, sb_x, content_y, 6, view_h);
          cairo_fill(cr);
          cairo_restore(cr);
          draw_scrollbar(app, cr, sb_x, content_y, view_h, app.cur_tab().content_h,
                         view_h, app.cur_tab().scroll_px, app.outline_r,
                         app.outline_g, app.outline_b);
        }
      } else {
        auto ph = phase("gridview");
        draw_grid_view(app, cr, content_x, content_y, content_w, view_h);
      }
    } else {
      const char* view_name =
          app.cur_tab().view_mode == ViewMode::List    ? "listview"
        : app.cur_tab().view_mode == ViewMode::Grid    ? "gridview"
        : app.cur_tab().view_mode == ViewMode::Computer? "computerview"
        : app.cur_tab().view_mode == ViewMode::Tree    ? "treeview"
        : app.cur_tab().view_mode == ViewMode::Compact ? "compactview"
                                                        : "content";
      auto ph = phase(view_name);
      if (app.cur_tab().view_mode == ViewMode::List) {
        draw_list_view(app, cr, content_x, content_y, content_w, view_h);
      } else if (app.cur_tab().view_mode == ViewMode::Grid) {
        draw_grid_view(app, cr, content_x, content_y, content_w, view_h);
      } else if (app.cur_tab().view_mode == ViewMode::Computer) {
        draw_computer_view(app, cr, content_x, content_y, content_w, view_h);
      } else if (app.cur_tab().view_mode == ViewMode::Tree) {
        draw_tree_view(app, cr, content_x, content_y, content_w, view_h);
      } else if (app.cur_tab().view_mode == ViewMode::Compact) {
        draw_compact_view(app, cr, content_x, content_y, content_w, view_h);
      }
    }
  }

  // Marquee selection rectangle
  if (app.marquee_active) {
    draw_marquee(app, cr);
  }

  cairo_restore(cr);  // restore content-area clip

  // Top bar
  if (top_h > 0)
    { auto ph = phase("topbar"); draw_top_bar(app, cr, w, top_h, 0); }

  // Search results banner (under top bar, above content)
  if (search_banner_on)
    draw_search_banner(app, cr, content_x, top_h + tab_h, content_w);

  // Tab bar (shifted down by top_h)
  cairo_save(cr);
  cairo_translate(cr, 0, top_h);
  cairo_rectangle(cr, 0, 0, w, tab_h);
  cairo_clip(cr);
  { auto ph = phase("tabbar"); draw_tab_bar(app, cr, w, tab_h, top_h); }
  cairo_restore(cr);

  // Status bar
  { auto ph = phase("statusbar"); draw_status_bar(app, cr, w, h, status_h); }

  // Adaptive sidebar fold flap (module-owned overlay paint).
  paint_sidebar_flap(app, cr, w, h, view_h, status_h);

  // Directory/file picker bar
  if (app.select_dir_mode || app.select_file_mode)
    draw_select_dir_bar(app, cr, w, h, selector_h);

  // Dialogs
  if (app.create_dialog_open) draw_create_dialog(app, cr);
  if (app.select_pattern_open) draw_select_pattern_dialog(app, cr);
  if (app.rename_ui_open) draw_rename_ui(app, cr);
  if (app.batch_rename_open) draw_batch_rename(app, cr);
  if (app.confirm_open) draw_confirm_dialog(app, cr);
  if (app.conflict_open) draw_conflict_dialog(app, cr);
  if (app.password_dialog_open) draw_password_dialog(app, cr);
  if (app.compress_dialog_open) draw_compress_dialog(app, cr);
  if (app.term_chooser_open) draw_terminal_chooser(app, cr);
  if (app.open_with_open) draw_open_with(app, cr);

  // Sort menu
  if (app.r_sort_menu_open || app.sort_menu_open) {
    app.active_pane = app.r_sort_menu_open ? 1 : 0;
    draw_sort_menu(app, cr);
  }

  // Column chooser
  if (app.r_columns_menu_open || app.columns_menu_open) {
    app.active_pane = app.r_columns_menu_open ? 1 : 0;
    draw_columns_menu(app, cr);
  }

  // Filter dropdown
  if (app.r_filter_dropdown_section > 0 || app.filter_dropdown_section > 0) {
    app.active_pane = app.r_filter_dropdown_section > 0 ? 1 : 0;
    draw_filter_dropdown(app, cr, app.active_pane ? app.r_filter_dropdown_section : app.filter_dropdown_section);
  }

  // Context menu
  if (app.context_menu_open)
    draw_context_menu(app, cr);

  // Preview popup (skipped when a separate layer‑shell surface is active)
  // and never painted over the right-click context menu.
  if (app.preview_active && !app.previewPopupSurface && !app.context_menu_open)
    draw_hover_preview(app, cr);

  // Info panel (F11)
  { auto ph = phase("infopanel"); draw_info_panel(app, cr); }

  // Operations panel (right sidebar)
  { auto ph = phase("opspanel"); draw_operations_panel(app, cr); }

  // Drop action chooser (Copy/Move prompt) — drawn last so it sits on top.
  if (app.drop_chooser_open) draw_drop_chooser(app, cr);

  // Hit-registry debug overlay: outlines every retained region so hit
  // recalculation can be verified visually at any window size.
  {
    static const bool on = [] {
      const char* e = std::getenv("EH_HIT_DEBUG");
      return e && *e && e[0] != '0';
    }();
    if (on) {
      for (size_t i = 0; i < app.hit_main.size(); ++i) {
        const auto& r = app.hit_main.at(i);
        cairo_set_source_rgba(cr, 1.0, 0.2, 0.9, 0.85);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);
        cairo_stroke(cr);
      }
    }
  }

  {
    static std::atomic<bool> logged_first{false};
    bool is_first_paint = !logged_first.exchange(true);
    if (is_first_paint && !slow_phases.empty() &&
        trace::enabled().load(std::memory_order_relaxed)) {
      for (auto& [n, ms] : slow_phases)
        trace::log("PAINT FIRST %s %.2f ms", n, ms);
      slow_phases.clear();
    }
  }
  if (!slow_phases.empty() && trace::enabled().load(std::memory_order_relaxed)) {
    for (auto& [n, ms] : slow_phases)
      trace::log("PAINT SLOW %s %.1f ms", n, ms);
  }
}

// ── main draw ────────────────────────────────────────────────────

void draw(AppState& app) {
  if (!app.surface) return;
  auto draw_t0 = std::chrono::steady_clock::now();
  struct DrawTimer {
    std::chrono::steady_clock::time_point t0;
    ~DrawTimer() {
      double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
      if (ms >= 50.0 && trace::enabled().load(std::memory_order_relaxed))
        trace::log("DRAW SLOW %.1f ms", ms);
    }
  } draw_timer{draw_t0};

  // Resize pacing: while a resize session is active and a frame callback is
  // still outstanding, don't start another repaint — the callback will.
  // Safety valve: proceed anyway if the compositor stalls >120 ms.
  if (app.resize_session_active && app.frame_cb &&
      std::chrono::steady_clock::now() - app.frame_cb_armed_at <
          std::chrono::milliseconds(120)) {
    app.pendingRedraw = true;
    return;
  }

  // Buffers don't match the configured size yet: only the main-loop path may
  // realloc + paint (frame callbacks can fire between configure and realloc).
  if (app.resize_buffers_dirty && app.shm) {
    app.pendingRedraw = true;
    return;
  }

  // Pick buffer
  int paint_bi = -1;
  for (int i = 0; i < 2; ++i) {
    if (!app.buf[i].busy()) { paint_bi = i; break; }
  }
  if (paint_bi < 0) {
    app.pendingRedraw = true;
    if (app.resize_session_active) ++app.resize_drops;
    return;
  }

  // Content scroll-delta reuse gate: this frame may shift last frame's
  // content column in place and repaint only the exposed band when the ONLY
  // change is the scroll offset. Requires grid mode (v1), a stable signature,
  // the buffer that holds last frame's content being free again, no overlays
  // that live above the content, and no split view.
  advance_scroll_render(app);
  ContentReuseHint reuse_hint = make_content_reuse_hint(app);

  if (reuse_hint.delta) paint_bi = app.content_reuse.last_bi;

  cairo_t* cr = app.buf[paint_bi].cairo();
  cairo_save(cr);

  if (!reuse_hint.delta) {
    // Clear
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  }

  paint(app, cr, reuse_hint.delta ? &reuse_hint : nullptr);

  // Record content-reuse state for the next frame.
  record_content_reuse(app, paint_bi);

  cairo_restore(cr);

  int w = app.width;
  int h = app.height;

  cairo_surface_flush(app.buf[paint_bi].cairo_surface());

  // Commit
  wl_surface_attach(app.surface, app.buf[paint_bi].wl(), 0, 0);
  wl_surface_damage_buffer(app.surface, 0, 0, w, h);
  app.buf[paint_bi].mark_busy();
  schedule_frame(app);
  wl_surface_commit(app.surface);
  if (app.wl.display()) wl_display_flush(app.wl.display());
}

// ── frame scheduling ─────────────────────────────────────────────

void schedule_frame(AppState& app) {
  if (!app.surface) return;

  bool need_anim =
      std::abs(app.cur_tab().scroll_smooth_current - app.cur_tab().scroll_smooth_target) > 0.5;
  if (!need_anim && app.split_view)
    need_anim =
      std::abs(app.right_pane.scroll_smooth_current - app.right_pane.scroll_smooth_target) > 0.5;

  // Operations panel slide animation
  if (!need_anim) {
    double ops_target = app.ops_panel_open ? 1.0 : 0.0;
    need_anim = std::abs(app.ops_panel_slide - ops_target) > 0.01;
  }

  // Resize sessions are paced to the compositor: one frame per callback.
  bool need_resize_pace = app.resize_session_active || app.resize_buffers_dirty;

  if (!need_anim && !app.scroll_needs_redraw && !need_resize_pace) return;

  if (app.frame_cb) return;

  static const wl_callback_listener kListener = {
    .done = [](void* data, wl_callback* cb, uint32_t) {
      wl_callback_destroy(cb);
      auto& a = *static_cast<AppState*>(data);
      a.frame_cb = nullptr;
      draw(a);
    }
  };

  app.frame_cb = wl_surface_frame(app.surface);
  wl_callback_add_listener(app.frame_cb, &kListener, &app);
  app.frame_cb_armed_at = std::chrono::steady_clock::now();
}

} // namespace eh::file_browser
