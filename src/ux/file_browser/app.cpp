#include "ux/file_browser/app.hpp"

#include <cairo/cairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include <unistd.h>

#include <sys/stat.h>

#include "configuration/shell_config.hpp"
#include "m3/core/primitives/box.hpp"

namespace eh::file_browser {

// ── main draw ────────────────────────────────────────────────────

void draw(AppState& app) {
  if (!app.surface) return;

  // Check if settings TOML changed and re-apply colors if so
  {
    static timespec s_last_mtime{};
    static bool s_initialized = false;
    const std::string toml_path = eh::config::state_settings_toml_path();
    struct stat st{};
    if (::stat(toml_path.c_str(), &st) == 0) {
      if (!s_initialized) {
        s_last_mtime = st.st_mtim;
        s_initialized = true;
      } else if (st.st_mtim.tv_sec != s_last_mtime.tv_sec ||
                 st.st_mtim.tv_nsec != s_last_mtime.tv_nsec) {
        s_last_mtime = st.st_mtim;
        reload_colors_from_config(app);
      }
    }
  }

  // Reset thumbnail decode budget for this frame
  app.thumb_decodes_this_frame = 0;
  app.thumb_pending_queue.clear();

  // Smooth scroll (tab content)
  bool tab_scrolling = std::abs(app.cur_tab().scroll_smooth_current - app.cur_tab().scroll_smooth_target) > 0.5;
  if (tab_scrolling) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    auto now_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                  static_cast<uint64_t>(ts.tv_nsec);
    double elapsed_ms =
        static_cast<double>(now_ns - app.scroll_anim_start_ns) / 1000000.0;
    constexpr double kTau = 40.0;
    double factor = 1.0 - std::exp(-elapsed_ms / kTau);
    double diff = app.cur_tab().scroll_smooth_target - app.cur_tab().scroll_smooth_current;
    app.cur_tab().scroll_smooth_current += diff * factor;
    if (std::abs(app.cur_tab().scroll_smooth_current - app.cur_tab().scroll_smooth_target) <= 0.5) {
      app.cur_tab().scroll_smooth_current = app.cur_tab().scroll_smooth_target;
    }
    app.cur_tab().scroll_px = static_cast<int>(std::lround(app.cur_tab().scroll_smooth_current));
    app.scroll_needs_redraw = true;
  } else {
    app.cur_tab().scroll_px = static_cast<int>(std::lround(app.cur_tab().scroll_smooth_target));
    app.cur_tab().scroll_smooth_current = app.cur_tab().scroll_smooth_target;
  }

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

  if (!tab_scrolling && !comp_scrolling) {
    app.scroll_needs_redraw = false;
  }

  // Pick buffer
  int paint_bi = -1;
  for (int i = 0; i < 2; ++i) {
    if (!app.buf[i].busy()) { paint_bi = i; break; }
  }
  if (paint_bi < 0) {
    app.pendingRedraw = true;
    return;
  }

  cairo_t* cr = app.buf[paint_bi].cairo();
  cairo_save(cr);

  // Clear
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  int w = app.width;
  int h = app.height;
  int top_h = app.top_bar_height;
  app.tab_bar_height = (app.tabs.size() > 1) ? 44 : 0;
  int tab_h = app.tab_bar_height;
  int status_h = app.status_bar_height;

  // Fit sidebar width to longest label
  if (app.sidebar_expanded && !app.sidebar_locations.empty()) {
    cairo_save(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    // Sidebar uses zf = 1.2 * sidebar_scale internally (sidebar defaults to 1.2×)
    cairo_set_font_size(cr, 13.0 * 1.2);
    double max_text_w = 0;
    for (const auto& loc : app.sidebar_locations) {
      cairo_text_extents_t te;
      cairo_text_extents(cr, loc.label.c_str(), &te);
      if (te.x_advance > max_text_w) max_text_w = te.x_advance;
    }
    cairo_restore(cr);
    // left: (icon_x 16 + icon_sz 20 + gap 12) * 1.2 = 57.6
    // right: sb_margin 8 * 1.2 = 9.6  → total padding ≈ 68
    int needed = static_cast<int>(std::ceil(68.0 + max_text_w));
    int new_base = std::max(150, needed);
    if (new_base != app.sidebar_width_base) {
      app.sidebar_width_base = new_base;
      app.sidebar_width = std::max(120, static_cast<int>(app.sidebar_width_base * app.zoom_pct / 100.0));
    }
  }

  int sidebar_w = app.sidebar_expanded ? app.sidebar_width : 0;
  int content_x = sidebar_w;
  int content_w = w - sidebar_w;
  int content_y = top_h + tab_h;
  int view_h = h - top_h - tab_h - status_h;

  double surf_alpha = app.surface_opacity_pct / 100.0;

  // Background (content column, from top to status bar — covers behind top
  // bar, tab bar, and content view.  Sidebar and status bar paint their own
  // backgrounds.)
  cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, surf_alpha);
  cairo_rectangle(cr, sidebar_w, 0, content_w, h - status_h);
  cairo_fill(cr);

  // Sidebar
  int sidebar_h = h - status_h;
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, sidebar_w, sidebar_h);
  cairo_clip(cr);
  double sidebar_alpha = app.sidebar_opacity_pct / 100.0;
  cairo_set_source_rgba(cr, app.surface_r * 2, app.surface_g * 2, app.surface_b * 2, sidebar_alpha);
  cairo_rectangle(cr, 0, 0, sidebar_w, sidebar_h);
  cairo_fill(cr);
  draw_sidebar(app, cr, sidebar_w, 0, view_h);
  cairo_restore(cr);

  // Sidebar separator (and resize handle)
  if (sidebar_w > 0) {
    int sep_h = h - status_h;
    if (app.sidebar_hover_resize) {
      cairo_set_source_rgba(cr, 0.4, 0.6, 1.0, 0.6);
      cairo_rectangle(cr, sidebar_w - 1, 0, 3, sep_h);
    } else {
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b,
                             0.3);
      cairo_rectangle(cr, sidebar_w, 0, 1, sep_h);
    }
    cairo_fill(cr);
  }

  // Content area
  cairo_save(cr);
  cairo_rectangle(cr, content_x, content_y, content_w, view_h);
  cairo_clip(cr);

  // Scrollbar
  if (app.cur_tab().view_mode == ViewMode::Computer) {
    draw_scrollbar(cr, w - 10, content_y, view_h, app.computer_content_h, view_h,
                    app.computer_scroll_px, app.outline_r, app.outline_g, app.outline_b);
  } else {
    draw_scrollbar(cr, w - 10, content_y, view_h, app.cur_tab().content_h, view_h,
                    app.cur_tab().scroll_px, app.outline_r, app.outline_g, app.outline_b);
  }

  // File listing
  if (app.cur_tab().view_mode == ViewMode::List) {
    draw_list_view(app, cr, content_x, content_y, content_w, view_h);
  } else if (app.cur_tab().view_mode == ViewMode::Grid) {
    draw_grid_view(app, cr, content_x, content_y, content_w, view_h);
  } else if (app.cur_tab().view_mode == ViewMode::Computer) {
    draw_computer_view(app, cr, content_x, content_y, content_w, view_h);
  }

  // Marquee selection rectangle (drawn within the content-area clip)
  if (app.marquee_active) {
    draw_marquee(app, cr);
  }

  cairo_restore(cr);

  // Top bar
  draw_top_bar(app, cr, w, top_h);

  // Tab bar (shifted down by top_h)
  cairo_save(cr);
  cairo_translate(cr, 0, top_h);
  cairo_rectangle(cr, 0, 0, w, tab_h);
  cairo_clip(cr);
  draw_tab_bar(app, cr, w, tab_h);
  cairo_restore(cr);

  // Status bar
  draw_status_bar(app, cr, w, h, status_h);

  // Create dialog
  if (app.create_dialog_open)
    draw_create_dialog(app, cr);

  // Rename UI dialog
  if (app.rename_ui_open)
    draw_rename_ui(app, cr);

  // Batch rename dialog
  if (app.batch_rename_open)
    draw_batch_rename(app, cr);

  // Confirm dialog
  if (app.confirm_open)
    draw_confirm_dialog(app, cr);

  // Compress dialog
  if (app.compress_dialog_open)
    draw_compress_dialog(app, cr);

  // Terminal chooser
  if (app.term_chooser_open)
    draw_terminal_chooser(app, cr);

  // Properties dialog
  if (app.properties.open)
    draw_properties_dialog(app, cr);

  // Open With dialog
  if (app.open_with_open)
    draw_open_with(app, cr);

  // Settings dialog
  if (app.settings_open)
    draw_settings_dialog(app, cr);

  // Sort menu (drawn on top of the top bar)
  if (app.sort_menu_open)
    draw_sort_menu(app, cr);

  // Filter dropdown (drawn on top of the top bar)
  if (app.filter_dropdown_section > 0)
    draw_filter_dropdown(app, cr, app.filter_dropdown_section);

  // Context menu (drawn on top of most things)
  if (app.context_menu_open)
    draw_context_menu(app, cr);

  // Preview popup (skipped when a separate layer‑shell surface is active)
  if (app.preview_active && !app.previewPopupSurface)
    draw_hover_preview(app, cr);

  cairo_restore(cr);
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

  if (!need_anim && !app.scroll_needs_redraw) return;

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
}

} // namespace eh::file_browser
