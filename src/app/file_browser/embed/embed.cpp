#include "app/file_browser/embed/embed.hpp"
#include "../trace.hpp"
#include "../app.hpp"
#include "app/file_browser/features/view_zoom.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <memory>

#include <thread>

#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

#include "config/shell_config.hpp"
#include "services/udisks2/udisks2_drive_service.hpp"
#include "app/file_browser/features/drag.hpp"
#include "app/file_browser/features/thumb_pool.hpp"
#include "app/file_browser/features/dir_stats.hpp"
#include "app/file_browser/features/dir_watch.hpp"
#include "base/thread/thread_dispatch.hpp"
#include "platform/common/asset/asset_loader.hpp"
#include "platform/common/bench/startup_trace.hpp"
#include "platform/common/palette/matugen_palette.hpp"
#include "wayland/core/connection.hpp"
#include "wayland/core/seat.hpp"
#include "wayland/buffer/shm_buffer.hpp"
#include "wayland/surface/surface_extensions.hpp"
#include "xdg-shell-client-protocol.h"

namespace eh::file_browser {

// ── globals ──────────────────────────────────────────────────────

static std::unique_ptr<AppState> g_app;

// ── signal handling ──────────────────────────────────────────────

namespace {
volatile sig_atomic_t g_signal{0};
void signal_handler(int) { g_signal = 1; }
}

// ── early blank frame ────────────────────────────────────────────

// Paint a flat background into the first free buffer and map it. Used only
// during cold start so the compositor shows the window before fonts/assets/
// the real scene are ready.
static void paint_blank_frame(AppState& app) {
  for (int i = 0; i < 2; ++i) {
    if (app.buf[i].busy()) continue;
    auto* cr = app.buf[i].cairo();
    if (!cr) return;
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, app.bg_r, app.bg_g, app.bg_b);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_surface_flush(app.buf[i].cairo_surface());
    wl_surface_attach(app.surface, app.buf[i].wl(), 0, 0);
    wl_surface_damage_buffer(app.surface, 0, 0, app.buf[i].width(),
                             app.buf[i].height());
    app.buf[i].mark_busy();
    wl_surface_commit(app.surface);
    if (app.wl.display()) wl_display_flush(app.wl.display());
    return;
  }
}

// ── Wayland listeners ────────────────────────────────────────────

static void xdg_wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) {
  xdg_wm_base_pong(wm, serial);
}
static constexpr xdg_wm_base_listener kXdgWmBaseListener{
  .ping = xdg_wm_base_ping,
};

void resize_session_dump(AppState& app);

static void xdg_surface_configure(void* data, xdg_surface* surface,
                                   uint32_t serial) {
  auto& app = *static_cast<AppState*>(data);
  xdg_surface_ack_configure(surface, serial);

  // Cold start: map with a flat background immediately; the full UI lands
  // once assets/fonts are warm (embed clears startup_blank_frame after).
  if (app.startup_blank_frame) {
    if (app.width > 0 && app.height > 0) {
      if (app.buf[0].width() != app.width || app.buf[0].height() != app.height) {
        app.buf[0].ensure(app.shm, "eh-fb-a", app.width, app.height);
        app.buf[1].ensure(app.shm, "eh-fb-b", app.width, app.height);
      }
      paint_blank_frame(app);
    }
    return;
  }

  if (app.width <= 0 || app.height <= 0) return;

  // Resize coalescing: NEVER paint inside dispatch. Record the new size and
  // let the main loop produce at most one paced frame per vsync.
  if ((app.last_paint_w != app.width || app.last_paint_h != app.height) &&
      app.shm) {
    app.resize_buffers_dirty = true;
    app.pendingRedraw = true;
    app.resize_last_size_change = std::chrono::steady_clock::now();
    return;
  }
  return;
}

void resize_session_dump(AppState& app) {
  trace::log(
      "RESIZE SESSION end: %d ticks, %d dropped frames | tick avg %.1f max "
      "%.1f ms | buffer realloc max %.2f ms | draw max %.2f ms",
      app.resize_ticks, app.resize_drops,
      app.resize_ticks ? app.resize_tick_ms_sum / app.resize_ticks : 0.0,
      app.resize_tick_ms_max, app.resize_buf_ms_max, app.resize_draw_ms_max);
  // Aggregate paint phases by name: count / sum / max
  std::map<std::string, std::array<double, 3>> agg; // n, sum, max
  for (auto& [name, ms] : app.resize_phase_samples) {
    auto& a = agg[name];
    a[0] += 1;
    a[1] += ms;
    a[2] = std::max(a[2], ms);
  }
  std::vector<std::pair<std::string, std::array<double, 3>>> rows(agg.begin(),
                                                                  agg.end());
  std::sort(rows.begin(), rows.end(),
            [](auto& l, auto& r) { return l.second[2] > r.second[2]; });
  for (auto& [name, a] : rows)
    trace::log("RESIZE PHASE %-14s n=%-4.0f sum=%8.1f max=%7.2f ms", name.c_str(),
               a[0], a[1], a[2]);
  app.resize_session_active = false;
}

static void toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h,
                                wl_array*) {
  auto& app = *static_cast<AppState*>(data);
  if (w > 0) app.width = w;
  if (h > 0) {
    app.height = h;
  }
}

static void toplevel_close(void* data, xdg_toplevel*) {
  auto& app = *static_cast<AppState*>(data);
  app.running = false;
}

static constexpr xdg_surface_listener kXdgSurfaceListener{
  .configure = xdg_surface_configure,
};

static constexpr xdg_toplevel_listener kToplevelListener{
  .configure = toplevel_configure,
  .close = toplevel_close,
  .configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {},
  .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};

// ── buffer release hook ──────────────────────────────────────────

static void on_buf_release_hook(void* user) {
  auto& app = *static_cast<AppState*>(user);
  if (!app.surface) return;
  app.pendingRedraw = false;
  draw(app);
}

// ── properties window forward declarations ───────────────────────
static void destroy_props_window_impl(AppState& app);
static void draw_props_window_impl(AppState& app);

static void on_props_buf_release_hook(void* user) {
  auto& app = *static_cast<AppState*>(user);
  if (!app.props_surface) return;
  if (app.props_pendingRedraw) {
    app.props_pendingRedraw = false;
    draw_props_window_impl(app);
  }
}

// ── properties window listeners ──────────────────────────────────

static void props_xdg_surface_configure(void* data, xdg_surface* surface,
                                         uint32_t serial) {
  auto& app = *static_cast<AppState*>(data);
  xdg_surface_ack_configure(surface, serial);

  if (app.props_width <= 0 || app.props_height <= 0) return;

  bool needs_resize = (app.props_width != app.props_buf[0].width() ||
                       app.props_height != app.props_buf[0].height());

  if (needs_resize && app.shm) {
    app.props_buf[0].ensure(app.shm, "eh-props-a", app.props_width, app.props_height);
    app.props_buf[1].ensure(app.shm, "eh-props-b", app.props_width, app.props_height);
  }

  draw_props_window_impl(app);
}

static void props_toplevel_configure(void* data, xdg_toplevel*,
                                      int32_t w, int32_t h, wl_array*) {
  auto& app = *static_cast<AppState*>(data);
  if (w > 0) app.props_width = w;
  if (h > 0) app.props_height = h;
}

static void props_toplevel_close(void* data, xdg_toplevel*) {
  auto& app = *static_cast<AppState*>(data);
  destroy_props_window_impl(app);
}

static constexpr xdg_surface_listener kPropsXdgSurfaceListener{
  .configure = props_xdg_surface_configure,
};

static constexpr xdg_toplevel_listener kPropsToplevelListener{
  .configure = props_toplevel_configure,
  .close = props_toplevel_close,
  .configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {},
  .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};

// ── properties window create / destroy / draw ────────────────────

static void destroy_props_window_impl(AppState& app) {
  if (!app.props_surface) return;
  for (auto& b : app.props_buf) b.destroy();
  if (app.props_toplevel) { xdg_toplevel_destroy(app.props_toplevel); app.props_toplevel = nullptr; }
  if (app.props_xdgSurface) { xdg_surface_destroy(app.props_xdgSurface); app.props_xdgSurface = nullptr; }
  wl_surface_destroy(app.props_surface);
  app.props_surface = nullptr;
  app.properties.open = false;
  app.props_pendingRedraw = false;
}

void destroy_props_window(AppState& app) {
  destroy_props_window_impl(app);
}

static void draw_props_window_impl(AppState& app) {
  if (!app.props_surface || !app.properties.open) return;

  int paint_bi = -1;
  for (int i = 0; i < 2; ++i) {
    if (!app.props_buf[i].busy()) { paint_bi = i; break; }
  }
  if (paint_bi < 0) {
    app.props_pendingRedraw = true;
    return;
  }

  int pw = app.props_width;
  int ph = app.props_height;

  cairo_t* cr = app.props_buf[paint_bi].cairo();
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
  app.pointerX = static_cast<double>(app.props_pointerX);
  app.pointerY = static_cast<double>(app.props_pointerY);

  draw_properties_dialog(app, cr);

  app.width = saved_w;
  app.height = saved_h;
  app.pointerX = saved_px;
  app.pointerY = saved_py;

  cairo_restore(cr);

  cairo_surface_flush(app.props_buf[paint_bi].cairo_surface());

  wl_surface_attach(app.props_surface, app.props_buf[paint_bi].wl(), 0, 0);
  wl_surface_damage_buffer(app.props_surface, 0, 0, pw, ph);
  app.props_buf[paint_bi].mark_busy();
  wl_surface_commit(app.props_surface);
  if (app.wl.display()) wl_display_flush(app.wl.display());
}

void draw_props_window(AppState& app) {
  draw_props_window_impl(app);
}

void create_props_window(AppState& app) {
  if (app.props_surface) {
    draw_props_window(app);
    return;
  }

  auto* display = app.wl.display();
  auto* comp = app.wl.compositor();
  auto* xdg = app.wl.xdg_base();
  if (!display || !comp || !xdg || !app.shm) return;

  app.props_surface = wl_compositor_create_surface(comp);
  if (!app.props_surface) return;

  app.props_xdgSurface = xdg_wm_base_get_xdg_surface(xdg, app.props_surface);
  if (!app.props_xdgSurface) {
    wl_surface_destroy(app.props_surface);
    app.props_surface = nullptr;
    return;
  }
  xdg_surface_add_listener(app.props_xdgSurface, &kPropsXdgSurfaceListener, &app);

  app.props_toplevel = xdg_surface_get_toplevel(app.props_xdgSurface);
  if (!app.props_toplevel) {
    xdg_surface_destroy(app.props_xdgSurface);
    app.props_xdgSurface = nullptr;
    wl_surface_destroy(app.props_surface);
    app.props_surface = nullptr;
    return;
  }
  xdg_toplevel_add_listener(app.props_toplevel, &kPropsToplevelListener, &app);
  xdg_toplevel_set_title(app.props_toplevel, "Properties");
  xdg_toplevel_set_app_id(app.props_toplevel, "horizon-files-properties");
  xdg_toplevel_set_min_size(app.props_toplevel, app.props_width, app.props_height);
  xdg_toplevel_set_max_size(app.props_toplevel, app.props_width, app.props_height);

  app.props_buf[0].ensure(app.shm, "eh-props-a", app.props_width, app.props_height);
  app.props_buf[1].ensure(app.shm, "eh-props-b", app.props_width, app.props_height);
  app.props_buf[0].set_release_hook(on_props_buf_release_hook, &app);
  app.props_buf[1].set_release_hook(on_props_buf_release_hook, &app);

  wl_surface_commit(app.props_surface);
  app.props_pendingRedraw = true;
}

void handle_props_click(AppState& app, int x, int y, int button) {
  if (button != 0x110) return; // left click only

  int hit = properties_hit_test(app, x, y);

  if (hit == -1 || hit == -2) {
    destroy_props_window_impl(app);
    return;
  }

  // Tab switch
  if (hit <= -10 && hit >= -13) {
    int new_tab = -(hit + 10);
    int num_tabs = 2;
    if (app.properties.image_w > 0 && app.properties.image_h > 0) ++num_tabs;
    if (app.properties.is_media) ++num_tabs;
    if (new_tab >= 0 && new_tab < num_tabs) {
      app.properties.tab = new_tab;
      app.properties.combo_open = -1;
      app.properties.scroll_px = 0;
    }
    app.props_pendingRedraw = true;
    return;
  }

  // Combo dropdown toggle
  if (hit >= 10 && hit <= 12) {
    int pi = hit - 10;
    if (app.properties.combo_open == pi)
      app.properties.combo_open = -1;
    else
      app.properties.combo_open = pi;
    app.props_pendingRedraw = true;
    return;
  }

  // Combo item selection
  if (hit >= 200 && hit < 212) {
    int idx = hit - 200;
    int pi = idx / 4;
    int ci = idx % 4;
    int* targets[3] = {&app.properties.perm_owner, &app.properties.perm_group, &app.properties.perm_other};
    *targets[pi] = ci;
    app.properties.combo_open = -1;

    mode_t mode = 0;
    mode |= (app.properties.perm_owner >= 1 ? S_IRUSR : 0);
    mode |= (app.properties.perm_owner >= 2 ? S_IWUSR : 0);
    mode |= (app.properties.perm_owner >= 3 ? S_IXUSR : 0);
    mode |= (app.properties.perm_group >= 1 ? S_IRGRP : 0);
    mode |= (app.properties.perm_group >= 2 ? S_IWGRP : 0);
    mode |= (app.properties.perm_group >= 3 ? S_IXGRP : 0);
    mode |= (app.properties.perm_other >= 1 ? S_IROTH : 0);
    mode |= (app.properties.perm_other >= 2 ? S_IWOTH : 0);
    mode |= (app.properties.perm_other >= 3 ? S_IXOTH : 0);
    mode |= (app.properties.current_mode & ~(S_IRWXU | S_IRWXG | S_IRWXO));

    if (app.properties.multi) {
      for (const auto& t : app.properties.paths) chmod(t.c_str(), mode);
    } else {
      chmod(app.properties.path.c_str(), mode);
    }
    app.properties.current_mode = mode;
    app.props_pendingRedraw = true;
    return;
  }

  // Executable toggle
  if (hit == 15) {
    app.properties.executable = !app.properties.executable;
    mode_t mode = app.properties.current_mode;
    if (app.properties.executable) {
      mode |= S_IXUSR | S_IXGRP | S_IXOTH;
    } else {
      mode &= ~(S_IXUSR | S_IXGRP | S_IXOTH);
    }
    if (app.properties.multi) {
      // Flip only the exec bits on each item, preserving individual modes
      for (const auto& t : app.properties.paths) {
        struct stat st;
        if (stat(t.c_str(), &st) != 0) continue;
        mode_t m = st.st_mode;
        if (app.properties.executable) m |= S_IXUSR | S_IXGRP | S_IXOTH;
        else m &= ~(S_IXUSR | S_IXGRP | S_IXOTH);
        chmod(t.c_str(), m);
      }
    } else {
      chmod(app.properties.path.c_str(), mode);
    }
    app.properties.current_mode = mode;
    app.props_pendingRedraw = true;
    return;
  }

  // Numeric octal mode editor — begin editing
  if (hit == 16) {
    app.properties.combo_open = -1;
    app.properties.tags_edit = false;
    app.properties.octal_edit = true;
    if (app.properties.octal_buf.empty()) {
      char ob[16];
      snprintf(ob, sizeof(ob), "%lo",
               static_cast<unsigned long>(app.properties.current_mode & 07777));
      app.properties.octal_buf = ob;
    }
    app.props_pendingRedraw = true;
    return;
  }

  // Tags row — begin editing
  if (hit == 17 && !app.properties.tags_edit) {
    app.properties.combo_open = -1;
    app.properties.octal_edit = false;
    app.properties.tags_edit = true;
    app.properties.tags_buf = app.properties.tags_value;
    app.props_pendingRedraw = true;
    return;
  }
  if (hit == 17) {
    app.props_pendingRedraw = true;
    return;
  }

  // Clicked elsewhere inside dialog — close any open combo / cancel edits
  if (app.properties.combo_open >= 0 || app.properties.octal_edit ||
      app.properties.tags_edit) {
    app.properties.combo_open = -1;
    app.properties.octal_edit = false;
    app.properties.tags_edit = false;
    app.props_pendingRedraw = true;
    return;
  }
}

// ── settings window forward declarations ─────────────────────────
static void destroy_settings_window_impl(AppState& app);
static void draw_settings_window_impl(AppState& app);

static void on_settings_buf_release_hook(void* user) {
  auto& app = *static_cast<AppState*>(user);
  if (!app.settings_surface) return;
  if (app.settings_pendingRedraw) {
    app.settings_pendingRedraw = false;
    draw_settings_window_impl(app);
  }
}

static void settings_xdg_surface_configure(void* data, xdg_surface* surface,
                                            uint32_t serial) {
  auto& app = *static_cast<AppState*>(data);
  xdg_surface_ack_configure(surface, serial);
  if (app.settings_win_width <= 0 || app.settings_win_height <= 0) return;
  bool needs_resize = (app.settings_win_width != app.settings_buf[0].width() ||
                       app.settings_win_height != app.settings_buf[0].height());
  if (needs_resize && app.shm) {
    app.settings_buf[0].ensure(app.shm, "eh-settings-a", app.settings_win_width, app.settings_win_height);
    app.settings_buf[1].ensure(app.shm, "eh-settings-b", app.settings_win_width, app.settings_win_height);
  }
  draw_settings_window_impl(app);
}

static void settings_toplevel_configure(void* data, xdg_toplevel*,
                                         int32_t w, int32_t h, wl_array*) {
  auto& app = *static_cast<AppState*>(data);
  if (w > 0) app.settings_win_width = w;
  if (h > 0) app.settings_win_height = h;
}

static void settings_toplevel_close(void* data, xdg_toplevel*) {
  auto& app = *static_cast<AppState*>(data);
  destroy_settings_window_impl(app);
}

static constexpr xdg_surface_listener kSettingsXdgSurfaceListener{
  .configure = settings_xdg_surface_configure,
};

static constexpr xdg_toplevel_listener kSettingsToplevelListener{
  .configure = settings_toplevel_configure,
  .close = settings_toplevel_close,
  .configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {},
  .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};

// ── settings window create / destroy / draw ──────────────────────

static void destroy_settings_window_impl(AppState& app) {
  if (!app.settings_surface) return;
  for (auto& b : app.settings_buf) b.destroy();
  if (app.settings_toplevel) { xdg_toplevel_destroy(app.settings_toplevel); app.settings_toplevel = nullptr; }
  if (app.settings_xdgSurface) { xdg_surface_destroy(app.settings_xdgSurface); app.settings_xdgSurface = nullptr; }
  wl_surface_destroy(app.settings_surface);
  app.settings_surface = nullptr;
  app.settings_open = false;
  app.settings_pendingRedraw = false;
  app.settings_slider_dragging = 0;
}

void destroy_settings_window(AppState& app) {
  destroy_settings_window_impl(app);
}

static void draw_settings_window_impl(AppState& app) {
  if (!app.settings_surface || !app.settings_open) return;

  int paint_bi = -1;
  for (int i = 0; i < 2; ++i) {
    if (!app.settings_buf[i].busy()) { paint_bi = i; break; }
  }
  if (paint_bi < 0) {
    app.settings_pendingRedraw = true;
    return;
  }

  int pw = app.settings_win_width;
  int ph = app.settings_win_height;

  cairo_t* cr = app.settings_buf[paint_bi].cairo();
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
  app.pointerX = static_cast<double>(app.settings_pointerX);
  app.pointerY = static_cast<double>(app.settings_pointerY);

  draw_settings_dialog(app, cr);

  app.width = saved_w;
  app.height = saved_h;
  app.pointerX = saved_px;
  app.pointerY = saved_py;

  cairo_restore(cr);

  cairo_surface_flush(app.settings_buf[paint_bi].cairo_surface());

  wl_surface_attach(app.settings_surface, app.settings_buf[paint_bi].wl(), 0, 0);
  wl_surface_damage_buffer(app.settings_surface, 0, 0, pw, ph);
  app.settings_buf[paint_bi].mark_busy();
  wl_surface_commit(app.settings_surface);
  if (app.wl.display()) wl_display_flush(app.wl.display());
}

void draw_settings_window(AppState& app) {
  draw_settings_window_impl(app);
}

void create_settings_window(AppState& app) {
  if (app.settings_surface) {
    draw_settings_window(app);
    return;
  }

  auto* display = app.wl.display();
  auto* comp = app.wl.compositor();
  auto* xdg = app.wl.xdg_base();
  if (!display || !comp || !xdg || !app.shm) return;

  app.settings_surface = wl_compositor_create_surface(comp);
  if (!app.settings_surface) return;

  app.settings_xdgSurface = xdg_wm_base_get_xdg_surface(xdg, app.settings_surface);
  if (!app.settings_xdgSurface) {
    wl_surface_destroy(app.settings_surface);
    app.settings_surface = nullptr;
    return;
  }
  xdg_surface_add_listener(app.settings_xdgSurface, &kSettingsXdgSurfaceListener, &app);

  app.settings_toplevel = xdg_surface_get_toplevel(app.settings_xdgSurface);
  if (!app.settings_toplevel) {
    xdg_surface_destroy(app.settings_xdgSurface);
    app.settings_xdgSurface = nullptr;
    wl_surface_destroy(app.settings_surface);
    app.settings_surface = nullptr;
    return;
  }
  xdg_toplevel_add_listener(app.settings_toplevel, &kSettingsToplevelListener, &app);
  xdg_toplevel_set_title(app.settings_toplevel, "Settings");
  xdg_toplevel_set_app_id(app.settings_toplevel, "horizon-files-settings");

  // Size the window to the active tab's content before pinning min==max.
  app.settings_win_width = settings_dialog_width();
  app.settings_win_height = settings_dialog_card_height(app);
  xdg_toplevel_set_min_size(app.settings_toplevel, app.settings_win_width, app.settings_win_height);
  xdg_toplevel_set_max_size(app.settings_toplevel, app.settings_win_width, app.settings_win_height);

  app.settings_buf[0].ensure(app.shm, "eh-settings-a", app.settings_win_width, app.settings_win_height);
  app.settings_buf[1].ensure(app.shm, "eh-settings-b", app.settings_win_width, app.settings_win_height);
  app.settings_buf[0].set_release_hook(on_settings_buf_release_hook, &app);
  app.settings_buf[1].set_release_hook(on_settings_buf_release_hook, &app);

  wl_surface_commit(app.settings_surface);
  app.settings_pendingRedraw = true;
}

static void settings_apply_slider(AppState& app, int hit, int x) {
  const int slider_x = 20 + 8;
  const int slider_w = 420 - 2 * 20 - 16;
  double pct = static_cast<double>(x - slider_x) / slider_w * 100.0;
  int val = std::clamp(static_cast<int>(pct), 0, 100);
  if (hit == -11) {
    app.settings_opacity_pct = val;
    app.surface_opacity_pct = val;
  } else if (hit == -13) {
    app.settings_sidebar_opacity_pct = val;
    app.sidebar_opacity_pct = val;
  } else if (hit == -14) {
    app.settings_topbar_opacity_pct = val;
    app.topbar_opacity_pct = val;
  } else if (hit == -15) {
    app.settings_statusbar_opacity_pct = val;
    app.statusbar_opacity_pct = val;
  } else if (hit == -17) {
    app.settings_preview_opacity_pct = val;
    app.preview_opacity_pct = val;
  } else if (hit == -19) {
    app.settings_dialog_opacity_pct = val;
    app.dialog_opacity_pct = val;
  } else if (hit == -20) {
    app.settings_properties_opacity_pct = val;
    app.properties_opacity_pct = val;
  } else if (hit == -23) {
    const double sc = 1.0 + 9.0 * (val / 100.0);
    app.settings_preview_scale = sc;
    app.preview_scale = sc;
    // Real time: resize an already-showing image preview.
    if (app.preview_active && app.preview_thumb && app.preview_entry_idx >= 0 &&
        !app.cur_tab().visible_entries.empty()) {
      const int vi = app.preview_entry_idx;
      if (vi >= 0 && vi < static_cast<int>(app.cur_tab().visible_entries.size())) {
        const int ri = app.cur_tab().visible_entries[vi];
        if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size()) &&
            app.cur_tab().entries[ri].type == FileType::Image) {
          const int tw = cairo_image_surface_get_width(app.preview_thumb);
          const int th = cairo_image_surface_get_height(app.preview_thumb);
            eh::file_browser::resize_active_image_preview(app);
            app.pendingRedraw = true;
        }
      }
    }
  }
}

// ── Settings dialog layout (single source of truth) ──────────────
//
// Vertical model (offsets from card top):
//   title bar 44 + 4 gap + tabs 36 + 12 gap  => content top at 96
//   per-tab rows (see settings_content_height)
//   18 gap + button row 30 + 20 bottom pad   => button zone = card_h - 50
static constexpr int kSetContentTop = 96;
static constexpr int kSetContentGap = 18;
static constexpr int kSetButtonZone = 50;

int settings_dialog_width() {
  return 420;
}

static int settings_content_height(const AppState& app) {
  switch (app.settings_tab) {
    case 0: {
      // zoom @0..26, folders toggle @40..62, terminal box @76..106,
      // independent-views toggle @118..140
      int h = 140;
      if (app.settings_dropdown_open) {
        const int visible = std::min<int>(app.settings_term_opts.size(), 6);
        h = std::max(h, 108 + visible * 28);
      }
      return h;
    }
    case 1:
      // seven opacity slider rows + matugen + color engine toggles;
      // last row top at content_top + 8*52 = 512 from card top
      return 438;
    default:
      return 56;  // preview scale slider
  }
}

int settings_dialog_card_height(const AppState& app) {
  return kSetContentTop + settings_content_height(app) + kSetContentGap + kSetButtonZone;
}

void update_settings_window_size(AppState& app) {
  const int w = settings_dialog_width();
  const int h = settings_dialog_card_height(app);
  if (w == app.settings_win_width && h == app.settings_win_height) return;
  app.settings_win_width = w;
  app.settings_win_height = h;
  if (!app.settings_toplevel || !app.settings_surface) return;
  xdg_toplevel_set_min_size(app.settings_toplevel, w, h);
  xdg_toplevel_set_max_size(app.settings_toplevel, w, h);
  wl_surface_commit(app.settings_surface);
  if (app.wl.display()) wl_display_flush(app.wl.display());
}

void handle_settings_click(AppState& app, int x, int y, int button) {
  if (button != 0x110) return;

  int saved_w = app.width;
  int saved_h = app.height;
  app.width = app.settings_win_width;
  app.height = app.settings_win_height;
  int hit = settings_hit_test(app, x, y);
  app.width = saved_w;
  app.height = saved_h;

  if (hit != -16) app.settings_zoom_editing = false;

  if (hit == -1) return;
  if (hit == -2 || hit == -7) {
    destroy_settings_window_impl(app);
    return;
  }
  if (hit == -3) {
    app.settings_tab = 0;
    update_settings_window_size(app);
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit == -4) {
    app.settings_tab = 1;
    update_settings_window_size(app);
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit == -22) {
    app.settings_tab = 2;
    update_settings_window_size(app);
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit == -8) {
    apply_zoom_pct(app, std::clamp(app.settings_zoom_pct - 10.0, 50.0, 200.0));
    app.pendingRedraw = true;
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit == -9) {
    apply_zoom_pct(app, std::clamp(app.settings_zoom_pct + 10.0, 50.0, 200.0));
    app.pendingRedraw = true;
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit == -10) {
    app.settings_folders_before_files = !app.settings_folders_before_files;
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit == -11 || hit == -13 || hit == -14 || hit == -15 || hit == -17 || hit == -19 || hit == -20 || hit == -23) {
    settings_apply_slider(app, hit, x);
    app.settings_slider_dragging = hit;
    app.settings_pendingRedraw = true;
    app.pendingRedraw = true;
    return;
  }
  if (hit == -12) {
    app.settings_dropdown_open = !app.settings_dropdown_open;
    update_settings_window_size(app);
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit == -18) {
    app.settings_matugen_theming = !app.settings_matugen_theming;
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit == -24) {
    app.settings_color_engine = !app.settings_color_engine;
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit == -21) {
    app.settings_independent_dir_views = !app.settings_independent_dir_views;
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit >= 0) {
    app.settings_default_term_idx = hit + app.settings_dropdown_scroll;
    app.settings_dropdown_open = false;
    update_settings_window_size(app);
    app.settings_pendingRedraw = true;
    return;
  }
  if (hit == -5) {
    settings_apply(app);
    destroy_settings_window_impl(app);
    return;
  }
  if (hit == -6) {
    settings_apply(app);
    app.settings_pendingRedraw = true;
    return;
  }
}

// ── connect globals ──────────────────────────────────────────────

static bool connect_globals(AppState& app) {
  auto* display = app.wl.display();
  if (!display) return false;

  struct Globals {
    wl_compositor* compositor = nullptr;
    xdg_wm_base* xdgBase = nullptr;
    wl_shm* shm = nullptr;
  } g;

  wl_registry* registry = wl_display_get_registry(display);
  if (!registry) return false;

  static constexpr wl_registry_listener kRegListener{
    .global = [](void* data, wl_registry* reg, uint32_t name,
                  const char* iface, uint32_t) {
      auto& gl = *static_cast<Globals*>(data);
      if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        gl.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
      } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        gl.xdgBase = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, 2));
        xdg_wm_base_add_listener(gl.xdgBase, &kXdgWmBaseListener, nullptr);
      } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        gl.shm = static_cast<wl_shm*>(
            wl_registry_bind(reg, name, &wl_shm_interface, 1));
      }
    },
    .global_remove = [](void*, wl_registry*, uint32_t) {},
  };

  wl_registry_add_listener(registry, &kRegListener, &g);
  wl_display_roundtrip(display);

  if (!g.compositor || !g.xdgBase || !g.shm) return false;

  app.shm = g.shm;
  app.buf[0].ensure(g.shm, "eh-fb-shm-a", app.width, app.height);
  app.buf[1].ensure(g.shm, "eh-fb-shm-b", app.width, app.height);
  app.buf[0].set_release_hook(on_buf_release_hook, &app);
  app.buf[1].set_release_hook(on_buf_release_hook, &app);

  // Bind seat — bind() eagerly creates pointer and keyboard objects
  // so we don't depend on the capabilities event having been received.
  if (app.wl.seat()) {
    app.seat.bind(app.wl.seat());
  }

  return true;
}

// ── create window ────────────────────────────────────────────────

static bool create_window(AppState& app) {
  auto* display = app.wl.display();
  if (!display) return false;

  // Re-bind with a simpler approach: scan registry
  struct WinGlobals {
    wl_compositor* comp = nullptr;
    xdg_wm_base* xdg = nullptr;
    wl_shm* shm = nullptr;
  } wg;

  wl_registry* reg = wl_display_get_registry(display);
  static constexpr wl_registry_listener kWinRegListener{
    .global = [](void* data, wl_registry* reg, uint32_t name,
                  const char* iface, uint32_t) {
      auto& gl = *static_cast<WinGlobals*>(data);
      if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        gl.comp = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
      } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        gl.xdg = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, 2));
        xdg_wm_base_add_listener(gl.xdg, &kXdgWmBaseListener, nullptr);
      } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        gl.shm = static_cast<wl_shm*>(
            wl_registry_bind(reg, name, &wl_shm_interface, 1));
      }
    },
    .global_remove = [](void*, wl_registry*, uint32_t) {},
  };
  wl_registry_add_listener(reg, &kWinRegListener, &wg);
  wl_display_roundtrip(display);
  wl_display_roundtrip(display); // second roundtrip to get all events

  if (!wg.comp || !wg.xdg || !wg.shm) return false;

  app.shm = wg.shm;
  app.surface = wl_compositor_create_surface(wg.comp);
  if (!app.surface) return false;

  app.xdgSurface = xdg_wm_base_get_xdg_surface(wg.xdg, app.surface);
  if (!app.xdgSurface) return false;
  xdg_surface_add_listener(app.xdgSurface, &kXdgSurfaceListener, &app);

  app.toplevel = xdg_surface_get_toplevel(app.xdgSurface);
  if (!app.toplevel) return false;
  xdg_toplevel_add_listener(app.toplevel, &kToplevelListener, &app);
  xdg_toplevel_set_title(app.toplevel, "Files");
  xdg_toplevel_set_app_id(app.toplevel, "horizon-files");
  // Below ~700px the top bar's right cluster would start crowding the nav
  // field; keep the window large enough for the chrome to stay unoverlapped.
  xdg_toplevel_set_min_size(app.toplevel, 680, 320);

  // Initialize SHM buffers
  app.buf[0].ensure(wg.shm, "eh-fb-a", app.width, app.height);
  app.buf[0].set_release_hook(on_buf_release_hook, &app);
  app.buf[1].ensure(wg.shm, "eh-fb-b", app.width, app.height);
  app.buf[1].set_release_hook(on_buf_release_hook, &app);

  wl_surface_commit(app.surface);

  return true;
}

// ── run standalone ───────────────────────────────────────────────

[[nodiscard]] int run_standalone(const std::string& initial_path) {
  const auto startup_t0 = std::chrono::steady_clock::now();
  auto startup_mark = [&](const char* what) {
    if (!trace::enabled().load(std::memory_order_relaxed)) return;
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - startup_t0)
                    .count();
    trace::log("STARTUP %s %.1f ms", what, ms);
  };
  g_app = std::make_unique<AppState>();
  startup_mark("appstate");
  AppState& app = *g_app;
  app.initial_navigate_path = initial_path;

  if (!app.wl.connect()) {
    std::cerr << "WAYLAND_DISPLAY not set or compositor unavailable.\n";
    return 1;
  }
  startup_mark("connect");

  if (!connect_globals(app)) {
    std::cerr << "Failed to connect Wayland globals.\n";
    return 1;
  }

  // Bind clipboard service (data-control protocol)
  {
    auto* extMgr = app.wl.ext_data_control_manager();
    auto* wlrMgr = app.wl.wlr_data_control_manager();
    if (extMgr) {
      app.clipboard.bind(extMgr, eh::wayland::ext_data_control_ops(), app.wl.seat(), app.wl.display());
    } else if (wlrMgr) {
      app.clipboard.bind(wlrMgr, eh::wayland::wlr_data_control_ops(), app.wl.seat(), app.wl.display());
    }
  }

  // Initialize data device for drag-and-drop (wl_data_device protocol)
  {
    auto* mgr = app.wl.data_device_manager();
    if (mgr) {
      app.data_device = wl_data_device_manager_get_data_device(mgr, app.wl.seat());
      if (app.data_device) {
        setup_drop_receiver(app);
      }
    }
  }

  startup_mark("globals");
  if (!create_window(app)) {
    std::cerr << "Failed to create file browser window.\n";
    return 1;
  }
  startup_mark("window");

  // Map the window NOW with a flat background frame: the initial commit
  // above makes the compositor send configure; one roundtrip receives it,
  // and the xdg handler paints a blank frame instead of the full UI.
  app.startup_blank_frame = true;
  wl_display_roundtrip(app.wl.display());
  startup_mark("mapped");
  // Start the icon worker now so the theme-index scan (~5-17 ms of file I/O)
  // overlaps SVG asset decoding instead of blocking the first sidebar draw.
  app.icons.tray_icon("user-home", 64);
  // Warm fonts/glyph caches on a helper thread concurrently with the asset,
  // settings, and sidebar work below (cairo/pango are internally locked).
  std::thread warm_thr([] { warmup_text_rendering(); });

  // Load arrow-icon SVGs at high resolution for crisp rendering at all zoom
  // levels. The six large rasters decode on a helper thread in parallel with
  // the small ones below (~2 ms off cold start).
  startup_mark("arrows_begin");
  static constexpr int kArrowIconLoadPx = 256;
  std::thread svg_big([&app] {
    app.arrow_left_svg    = eh::shell::asset::load_asset_svg("UI", "arrow-left.svg", kArrowIconLoadPx);
    app.arrow_right_svg   = eh::shell::asset::load_asset_svg("UI", "arrow-right.svg", kArrowIconLoadPx);
    app.arrow_up_svg      = eh::shell::asset::load_asset_svg("UI", "arrow-up.svg", kArrowIconLoadPx);
    app.search_svg        = eh::shell::asset::load_asset_svg("UI", "search.svg", kArrowIconLoadPx);
    app.folder_search_svg = eh::shell::asset::load_asset_svg("UI", "folder-search.svg", kArrowIconLoadPx);
  app.view_list_svg     = eh::shell::asset::load_asset_svg("UI", "view-list.svg", kArrowIconLoadPx);
  app.view_grid_svg     = eh::shell::asset::load_asset_svg("UI", "view-grid.svg", kArrowIconLoadPx);
  app.view_compact_svg  = eh::shell::asset::load_asset_svg("UI", "view-compact.svg", kArrowIconLoadPx);
  app.view_tree_svg     = eh::shell::asset::load_asset_svg("UI", "view-tree.svg", kArrowIconLoadPx);
  app.settings_gear_svg = eh::shell::asset::load_asset_svg("UI", "settings-gear.svg", kArrowIconLoadPx);
  app.three_dots_svg    = eh::shell::asset::load_asset_svg("UI", "three-dots.svg", kArrowIconLoadPx);
  app.sidebar_toggle_svg = eh::shell::asset::load_asset_svg("UI", "layout.svg", kArrowIconLoadPx);
  app.sort_chevron_svg  = eh::shell::asset::load_asset_svg("UI", "chevron-down.svg", kArrowIconLoadPx);
  app.checkmark_svg     = eh::shell::asset::load_asset_svg("UI", "checkmark.svg", kArrowIconLoadPx);
  app.arrow_down_svg    = eh::shell::asset::load_asset_svg("UI", "arrow-down.svg", kArrowIconLoadPx);
  app.arrow_downward_svg= eh::shell::asset::load_asset_svg("UI", "arrow-downward.svg", kArrowIconLoadPx);
  app.icon_hash_svg     = eh::shell::asset::load_asset_svg("UI", "hash.svg", kArrowIconLoadPx);
  app.icon_bars_svg     = eh::shell::asset::load_asset_svg("UI", "bar-chart-2.svg", kArrowIconLoadPx);
  app.icon_clock_svg    = eh::shell::asset::load_asset_svg("UI", "clock.svg", kArrowIconLoadPx);
  app.icon_file_text_svg= eh::shell::asset::load_asset_svg("UI", "file-text.svg", kArrowIconLoadPx);
  app.icon_person_svg   = eh::shell::asset::load_asset_svg("UI", "person.svg", kArrowIconLoadPx);
  app.icon_people_svg   = eh::shell::asset::load_asset_svg("UI", "people.svg", kArrowIconLoadPx);
  app.icon_shield_svg   = eh::shell::asset::load_asset_svg("UI", "shield.svg", kArrowIconLoadPx);
  app.icon_file_svg     = eh::shell::asset::load_asset_svg("UI", "file.svg", kArrowIconLoadPx);
  app.icon_link_svg     = eh::shell::asset::load_asset_svg("UI", "link.svg", kArrowIconLoadPx);
  app.icon_folder_svg   = eh::shell::asset::load_asset_svg("UI", "folder.svg", kArrowIconLoadPx);
  app.icon_eyeoff_svg   = eh::shell::asset::load_asset_svg("UI", "eye-off.svg", kArrowIconLoadPx);
  app.icon_list_svg     = eh::shell::asset::load_asset_svg("UI", "list.svg", kArrowIconLoadPx);
  app.icon_aa_svg       = eh::shell::asset::load_asset_svg("UI", "text.svg", kArrowIconLoadPx);
  app.icon_minus_svg    = eh::shell::asset::load_asset_svg("UI", "minus.svg", kArrowIconLoadPx);
  app.edit_svg          = eh::shell::asset::load_asset_svg("UI", "edit-2.svg", kArrowIconLoadPx);
  app.lock_svg          = eh::shell::asset::load_asset_svg("UI", "lock.svg", kArrowIconLoadPx);
  app.trash_svg         = eh::shell::asset::load_asset_svg("UI", "trash.svg", kArrowIconLoadPx);
  app.monitor_svg       = eh::shell::asset::load_asset_svg("UI", "monitor.svg", kArrowIconLoadPx);
  app.home_nav_svg      = eh::shell::asset::load_asset_svg("UI", "home.svg", kArrowIconLoadPx);
  app.music_nav_svg     = eh::shell::asset::load_asset_svg("UI", "music.svg", kArrowIconLoadPx);
  app.video_nav_svg     = eh::shell::asset::load_asset_svg("UI", "video.svg", kArrowIconLoadPx);
  app.documents_nav_svg = eh::shell::asset::load_asset_svg("UI", "documents.svg", kArrowIconLoadPx);
    app.mounted_svg       = eh::shell::asset::load_asset_svg("UI", "Mounted.svg", kArrowIconLoadPx);
  });
  app.icon_desktop_svg    = eh::shell::asset::load_asset_svg("UI", "icon-desktop.svg", 64);
  app.icon_documents_svg  = eh::shell::asset::load_asset_svg("UI", "icon-documents.svg", 64);
  app.icon_downloads_svg  = eh::shell::asset::load_asset_svg("UI", "icon-downloads.svg", 64);
  app.icon_music_svg      = eh::shell::asset::load_asset_svg("UI", "icon-music.svg", 64);
  app.icon_pictures_svg   = eh::shell::asset::load_asset_svg("UI", "icon-pictures.svg", 64);
  app.icon_videos_svg     = eh::shell::asset::load_asset_svg("UI", "icon-videos.svg", 64);
  app.icon_publicshare_svg  = eh::shell::asset::load_asset_svg("UI", "icon-publicshare.svg", 64);
  app.icon_templates_svg    = eh::shell::asset::load_asset_svg("UI", "icon-templates.svg", 64);
  svg_big.join();
  startup_mark("svg_assets");
  if (!app.arrow_left_svg || !app.arrow_right_svg || !app.arrow_up_svg || !app.search_svg || !app.folder_search_svg) {
    std::fprintf(stderr, "[horizon-files] WARNING: arrow/search SVGs not loaded (L=%p R=%p U=%p S=%p FS=%p). "
                         "CWD=%s\n",
                 (void*)app.arrow_left_svg, (void*)app.arrow_right_svg, (void*)app.arrow_up_svg,
                 (void*)app.search_svg, (void*)app.folder_search_svg,
                 []{ static char buf[4096]; return getcwd(buf, sizeof(buf)) ? buf : "?"; }());
  }

  // Initialize icon theme from shell config (e.g., MacTahoe-dark)
  {
    const auto& sc = eh::config::shell_config_snapshot_skip_matugen();
    if (!sc.dock.iconTheme.empty()) {
      app.icons.set_icon_theme(sc.dock.iconTheme);
      app.last_icon_theme = sc.dock.iconTheme;
    }
    app.icons.prewarm_search_dirs();
  }

  startup_mark("icon_theme");
  // Colors are set by reload_settings_from_config below

  // Load file browser settings from config
  reload_settings_from_config(app);
  startup_mark("settings");

  // Start UDisks2 drive service (background D-Bus event loop) — needs
  // to be running before refresh_sidebar() so query_drives() succeeds.
  drives::UDisks2DriveService::instance().start();
  drives::UDisks2DriveService::instance().set_change_callback([&app]() {
    app.sidebar_needs_refresh = true;
    app.pendingRedraw = true;
  });

  startup_mark("udisks");
  // Initialize
  refresh_sidebar(app);
  startup_mark("sidebar");
  app.startup_loading = true;
  if (!app.initial_navigate_path.empty()) {
    navigate_to(app, app.initial_navigate_path);
  } else {
    reload_dir(app);
  }
  startup_mark("reload_dir");

  // Wire up input event callbacks
  if (app.seat.pointer()) {
    app.seat.set_pointer_motion_cb(
        [&app](wl_surface* surface, double x, double y) {
          app.focused_surface = surface;
          if (surface == app.settings_surface) {
            app.settings_pointerX = static_cast<int>(x);
            app.settings_pointerY = static_cast<int>(y);
            if (app.settings_slider_dragging != 0) {
              settings_apply_slider(app, app.settings_slider_dragging,
                                    static_cast<int>(x));
              app.settings_pendingRedraw = true;
              app.pendingRedraw = true;
            } else {
              app.settings_pendingRedraw = true;
            }
          } else if (surface == app.props_surface) {
            app.props_pointerX = static_cast<int>(x);
            app.props_pointerY = static_cast<int>(y);
            app.props_pendingRedraw = true;
          } else {
            app.pointerX = x;
            app.pointerY = y;
            handle_pointer_move(app, static_cast<int>(x), static_cast<int>(y));
          }
        });
    app.seat.set_pointer_button_cb(
        [&app](uint32_t button, uint32_t state) {
          if (app.focused_surface == app.settings_surface) {
            if (state == 1) {
              handle_settings_click(app, app.settings_pointerX,
                                    app.settings_pointerY,
                                    static_cast<int>(button));
            } else {
              app.settings_slider_dragging = 0;
            }
            return;
          }
          if (app.focused_surface == app.props_surface) {
            if (state == 1) {
              handle_props_click(app, app.props_pointerX,
                                 app.props_pointerY,
                                 static_cast<int>(button));
            }
            return;
          }
          if (state == 1) {
            handle_click(app, static_cast<int>(app.pointerX),
                         static_cast<int>(app.pointerY),
                         static_cast<int>(button));
          } else {
            handle_pointer_release(app, static_cast<int>(app.pointerX),
                                   static_cast<int>(app.pointerY),
                                   static_cast<int>(button));
          }
        });
    app.seat.set_pointer_axis_vertical_cb(
        [&app](double delta_px) {
          if (app.focused_surface == app.settings_surface) {
            return;
          }
          if (app.focused_surface == app.props_surface) {
            app.properties.scroll_px += static_cast<int>(-delta_px);
            if (app.properties.scroll_px < 0) app.properties.scroll_px = 0;
            app.props_pendingRedraw = true;
            return;
          }
          handle_scroll(app, static_cast<int>(app.pointerX),
                        static_cast<int>(app.pointerY), 0.0, delta_px);
        });
  }
  if (app.seat.keyboard()) {
    app.seat.set_keyboard_key_cb(
        [&app](const eh::wayland::WaylandSeat::KeyboardEvent& ev) {
          int len = std::min(ev.utf8_len, static_cast<int>(ev.utf8.size()) - 1);
          handle_key(app, ev.keycode, ev.state, ev.sym, ev.utf8.data(), len);
        });
  }

  // Signal handling
  g_signal = 0;
  {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
  }

  std::cout << "Event Horizon File Browser started. Current path: "
            << app.cur_tab().current_path << "\n";

  warm_thr.join();
  startup_mark("warmup");
  // Initial draw (fonts warmed above so first paint never stalls)
  draw(app);
  app.startup_blank_frame = false;
  startup_mark("first_draw");
  app.startup_loading = false;
  // Keep painting briefly so background-resolved sidebar/MIME icons pop in
  // without waiting for user input or the cursor-blink tick.
  app.icon_catchup_frames = 30;

  // Event loop
  const int dpy_fd = wl_display_get_fd(app.wl.display());
  constexpr int kPollMs = 200;

  // Track settings file mtimes so we only re-read when they change
  int64_t toml_mtime = 0;
  int64_t ini_mtime = 0;
  int64_t fb_toml_mtime = 0;
  int64_t shell_color_mtime = 0;
  int64_t dev_disk_mtime = 0;
  auto last_progress_apply = std::chrono::steady_clock::now() -
                             std::chrono::milliseconds(1000);

  eh::file_browser::thumb_pool_start(&app);
  eh::file_browser::dir_stats_start();
  // Directed inotify watcher: reload on create/delete/move, re-stat changed
  // children in place (modify/attrib) so a rebuilt binary flips type/icon
  // while the folder stays open.
  app.dir_watch_fd = eh::file_browser::dir_watch_init();
  dir_watch_sync(app);
  while (app.running && g_signal == 0) {
    // ── check for external file changes on every iteration ────────
    if (!app.confirm_open && !app.create_dialog_open &&
        !app.settings_open && !app.open_with_open &&
        !app.term_chooser_open && !app.context_menu_open) {
      bool need_redraw = false;

      // Settings sync — stat the config files and reload when they change
      {
        bool settings_changed = false;
        struct stat st{};

        if (stat(eh::config::state_settings_toml_path().c_str(), &st) == 0) {
          int64_t mt = static_cast<int64_t>(st.st_mtime);
          if (mt != toml_mtime) { toml_mtime = mt; settings_changed = true; }
        }
        if (stat(eh::config::legacy_ini_path().c_str(), &st) == 0) {
          int64_t mt = static_cast<int64_t>(st.st_mtime);
          if (mt != ini_mtime) { ini_mtime = mt; settings_changed = true; }
        }
        if (stat(eh::config::state_file_browser_toml_path().c_str(), &st) == 0) {
          int64_t mt = static_cast<int64_t>(st.st_mtime);
          if (mt != fb_toml_mtime) { fb_toml_mtime = mt; settings_changed = true; }
        }

        {
          const std::string sc_path = eh::matugen::shell_color_config_path();
          if (stat(sc_path.c_str(), &st) == 0) {
            int64_t mt = static_cast<int64_t>(st.st_mtime);
            if (mt != shell_color_mtime) { shell_color_mtime = mt; settings_changed = true; }
          }
        }

        if (settings_changed) {
          reload_settings_from_config(app);
          need_redraw = true;
        }
      }

      // Drive list refresh — check /dev/disk mtime as fallback for non-UDisks2
      {
        struct stat ds{};
        if (stat("/dev/disk", &ds) == 0) {
          int64_t mt = static_cast<int64_t>(ds.st_mtime);
          if (mt != dev_disk_mtime) {
            dev_disk_mtime = mt;
            app.sidebar_needs_refresh = true;
            need_redraw = true;
          }
        }
      }

      // Apply finished background directory scans
      if (app.scan_ready_flag.load(std::memory_order_acquire)) {
        apply_scan_result(app);
        need_redraw = true;
      } else if (app.scan_progress_flag.load(std::memory_order_acquire) &&
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - last_progress_apply)
                         .count() >= 150.0) {
        apply_scan_result(app, /*is_progress=*/true);
        last_progress_apply = std::chrono::steady_clock::now();
        need_redraw = true;
      }

      // Install finished background thumbnails (UI thread owns the cache).
      {
        std::vector<eh::file_browser::ThumbBgResult> thumbs;
        eh::file_browser::thumb_pool_drain(app, thumbs);
        for (auto& t : thumbs) {
          if (eh::file_browser::preview_dbg_enabled()) {
            const int w = t.surface ? cairo_image_surface_get_width(t.surface) : 0;
            const int h = t.surface ? cairo_image_surface_get_height(t.surface) : 0;
            fprintf(stderr, "[preview] decode '%s' %dx%d\n", t.path.c_str(), w, h);
          }
          eh::file_browser::thumb_cache_install(app, t.path, t.size,
                                                t.surface);
        }
        if (!thumbs.empty()) need_redraw = true;
        if (eh::file_browser::dir_stats_drain(app)) need_redraw = true;
      }

      // Directory content refresh (active pane, plus the inactive split pane).
      // Re-arm the inotify watches first so drained events are attributed to
      // whatever the active tab and split pane are CURRENTLY viewing.
      const bool dir_watched = dir_watch_sync(app);

      // inotify is drained EVERY iteration, even while a background scan is
      // in flight, so a structural event is never left sitting in the kernel
      // buffer behind the current scan. The download case: creating `x.part`
      // fires a create->reload (launches a scan), and if the finish-rename
      // to `x` lands while that scan runs, we latch it below instead of
      // dropping it — the pane then reloads once the scan slot frees.
      // Relaunching the single background scan slot stays gated: stealing it
      // would cancel a pending navigation.
      if (app.dir_watch_fd >= 0) {
        struct pollfd ipf{};
        ipf.fd = app.dir_watch_fd;
        ipf.events = POLLIN | POLLERR | POLLHUP;
        const bool ifd_ready =
            poll(&ipf, 1, 0) > 0 && (ipf.revents & (POLLIN | POLLERR));

        bool need_reload_tab = false, need_reload_pane = false;
        dir_watch_process(app, ifd_ready, need_reload_tab,
                          need_reload_pane);

        if (need_reload_tab) app.dir_watch_tab_reload_pending = true;
        if (need_reload_pane && app.split_view)
          app.dir_watch_pane_reload_pending = true;

        // In-place child re-stat is cheap and scan-independent: a download's
        // size keeps ticking live even while the folder listing is scanning.
        if (dir_watch_refresh_entries(app)) need_redraw = true;
      }

      if (app.scan_active_path.empty() && !app.scan_apply_deferred) {
        // Honor structural reloads latched while the scan slot was busy.
        // reload_dir targets app.cur_tab(), so pin the right pane index
        // around each call — the deferred scan result is tagged with the
        // requesting pane and lands there after active_pane is restored.
        auto reload_pane_pinned = [&app](int pane) -> bool {
          const bool flip = app.split_view && app.active_pane != pane;
          if (flip) app.active_pane = pane;
          int saved_scroll = app.cur_tab().scroll_px;
          int saved_selected = app.cur_tab().selected_idx;
          reload_dir(app);
          app.cur_tab().scroll_px = saved_scroll;
          app.cur_tab().scroll_smooth_current =
              static_cast<double>(saved_scroll);
          app.cur_tab().scroll_smooth_target =
              static_cast<double>(saved_scroll);
          app.cur_tab().selected_idx = saved_selected;
          if (flip) app.active_pane = (pane == 0 ? 1 : 0);
          return true;
        };

        bool inotify_refreshed = false;
        if (app.dir_watch_tab_reload_pending) {
          app.dir_watch_tab_reload_pending = false;
          reload_pane_pinned(0);
          inotify_refreshed = true;
        }
        if (app.dir_watch_pane_reload_pending) {
          app.dir_watch_pane_reload_pending = false;
          reload_pane_pinned(1);
          inotify_refreshed = true;
        }
        if (inotify_refreshed) need_redraw = true;

        // stat() fallback only where inotify isn't covering the active
        // folder (NFS mounts, exhausted watch limits, unavailable kernels).
        if (!dir_watched) {
          auto refresh_pane = [&app]() -> bool {
            struct stat dir_st;
            if (::stat(app.cur_tab().current_path.c_str(), &dir_st) != 0)
              return false;
            int64_t new_mtime = static_cast<int64_t>(dir_st.st_mtime);
            if (new_mtime == app.cur_tab().dir_mtime) return false;
            int saved_scroll = app.cur_tab().scroll_px;
            int saved_selected = app.cur_tab().selected_idx;

            reload_dir(app);

            app.cur_tab().scroll_px = saved_scroll;
            app.cur_tab().scroll_smooth_current = static_cast<double>(saved_scroll);
            app.cur_tab().scroll_smooth_target = static_cast<double>(saved_scroll);
            app.cur_tab().selected_idx = saved_selected;
            return true;
          };

          bool refreshed = refresh_pane();
          if (app.split_view && app.active_pane == 0) {
            // Inactive right pane: reload_dir targets the active pane, so
            // flip panes around the call to refresh it too. The scan result
            // is tagged with the requesting pane, so a deferred apply still
            // lands in the right pane after active_pane is restored.
            app.active_pane = 1;
            refreshed = refresh_pane() || refreshed;
            app.active_pane = 0;
          }
          if (refreshed) need_redraw = true;
        }
      }

      if (need_redraw) draw(app);
    }

    // ── deferred sidebar refresh (avoids synchronous D-Bus inside event dispatch) ──
    if (app.sidebar_needs_refresh) {
      app.sidebar_needs_refresh = false;
      refresh_sidebar(app);
      if (!app.mount_navigate_drive_id.empty()) {
        for (auto& loc : app.sidebar_locations) {
          if (loc.kind == SidebarLocation::Kind::Drive &&
              loc.drive_id == app.mount_navigate_drive_id && loc.is_mounted) {
            navigate_to(app, loc.path);
            break;
          }
        }
        app.mount_navigate_drive_id.clear();
      }
      app.pendingRedraw = true;
    }

    // ── poll Wayland display fd ───────────────────────────────────
    // The inotify fd is polled alongside so a filesystem change (e.g. a
    // download finishing) wakes the loop immediately instead of on the next
    // 200 ms timeout — the listing then updates within a frame. It is left
    // out under a modal dialog so an undrained backlog of file events can't
    // make the loop spin: those are served once the dialog closes.
    struct pollfd pfds[2];
    pfds[0].fd = dpy_fd;
    pfds[0].events = POLLIN | POLLERR | POLLHUP;
    unsigned nfds = 1;
    if (app.dir_watch_fd >= 0 && !app.confirm_open && !app.create_dialog_open &&
        !app.settings_open && !app.open_with_open && !app.term_chooser_open &&
        !app.context_menu_open) {
      pfds[1].fd = app.dir_watch_fd;
      pfds[1].events = POLLIN | POLLERR | POLLHUP;
      nfds = 2;
    }

    bool search_pending = ((app.search_active || app.recursive_search_active) && !app.search_query.empty()) || ((app.r_search_active || app.r_recursive_search_active) && !app.r_search_query.empty());
    bool mount_wake = app.mount_poll_wake.exchange(false, std::memory_order_acq_rel);
    bool op_active = app.op_progress && app.op_progress->active.load();
    if (app.icon_catchup_frames > 0) {
      --app.icon_catchup_frames;
      app.pendingRedraw = true;
      // Catchup exists so background-resolved icons pop in; once nothing is
      // pending the remaining forced frames are pure waste — stop early.
      if (app.icons.pending_count() == 0) app.icon_catchup_frames = 0;
    }
    int poll_ms = (!app.thumb_pending_queue.empty() || search_pending || app.key_repeat_sym != 0 || mount_wake || op_active || app.icon_catchup_frames > 0) ? 0 : kPollMs;
    // A deferred scan apply must never sit on a 200 ms poll nap.
    if (poll_ms > 5 && app.scan_apply_deferred) poll_ms = 5;
    int pr = poll(pfds, nfds, poll_ms);
    if (pr < 0) {
      if (errno == EINTR) {
        if (g_signal != 0) break;
        continue;
      }
      break;
    }
    if (g_signal != 0) break;
    if (pfds[0].revents & (POLLERR | POLLHUP)) break;

    if (pfds[0].revents & POLLIN) {
      auto disp_t0 = std::chrono::steady_clock::now();
      uint64_t ev0 = eh::wayland::WaylandSeat::pointer_event_count();
      if (wl_display_dispatch(app.wl.display()) < 0) break;
      double disp_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - disp_t0)
                           .count();
      // Click handling runs inside dispatch, so long stalls here directly
      // hurt input latency — make them visible.
      if (disp_ms >= 100.0 && trace::enabled().load(std::memory_order_relaxed))
        trace::log("DISPATCH SLOW %.1f ms events=%llu", disp_ms,
                   (unsigned long long)(eh::wayland::WaylandSeat::pointer_event_count() -
                                        ev0));
    } else {
      wl_display_flush(app.wl.display());
    }

    // Measure only the WORK portion of this iteration (poll wait excluded).
    auto work_t0 = std::chrono::steady_clock::now();
    struct WorkTimer {
      std::chrono::steady_clock::time_point t0;
      const AppState* app;
      ~WorkTimer() {
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
        if (ms >= 25.0 && trace::enabled().load(std::memory_order_relaxed)) {
          trace::log("WORK SLOW %.1f ms entries=%zu visible=%zu mode=%d "
                     "thumbs_q=%zu precache=%zu/%zu status='%s'",
                     ms, app->cur_tab().entries.size(),
                     app->cur_tab().visible_entries.size(),
                     (int)app->cur_tab().view_mode,
                     app->thumb_pending_queue.size(),
                     app->precache_idx, app->precache_paths.size(),
                     app->operation_status.c_str());
        }
      }
    } work_timer{work_t0, &app};

    // ── process pending drive mount results ──────────────────────
    {
      std::string result_id;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(app.mount_mtx);
        if (!app.mount_result_drive_id.empty()) {
          result_id = std::move(app.mount_result_drive_id);
          app.mount_result_drive_id.clear();
          ok = app.mount_success;
        }
      }
      if (!result_id.empty()) {
        if (ok) {
          app.mount_navigate_drive_id = result_id;
          app.sidebar_needs_refresh = true;
          app.computer_needs_refresh = true;
        }
        app.pendingRedraw = true;
      }
    }

    // ── process pending drive unmount results ────────────────────
    {
      std::string result_id;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(app.mount_mtx);
        if (!app.unmount_result_drive_id.empty()) {
          result_id = std::move(app.unmount_result_drive_id);
          app.unmount_result_drive_id.clear();
          ok = app.unmount_success;
        }
      }
      if (!result_id.empty()) {
        if (ok) {
          // If we're viewing a directory on the unmounted drive, go home
          for (const auto& loc : app.sidebar_locations) {
            if (loc.kind == SidebarLocation::Kind::Drive &&
                loc.drive_id == result_id && loc.path != "/" &&
                loc.path.size() > 1) {
              if (app.cur_tab().current_path.size() >= loc.path.size() &&
                  app.cur_tab().current_path.compare(0, loc.path.size(), loc.path) == 0 &&
                  (app.cur_tab().current_path.size() == loc.path.size() ||
                   app.cur_tab().current_path[loc.path.size()] == '/')) {
                navigate_to(app, home_dir());
              }
              break;
            }
          }
          app.sidebar_needs_refresh = true;
          app.computer_needs_refresh = true;
        }
        app.pendingRedraw = true;
      }
    }

    // ── process pending thumbnail decodes ─────────────────────────
    if (process_pending_thumbnails(app)) {
      app.pendingRedraw = true;
    }

    // ── hover preview timer check ─────────────────────────────────
    check_hover_preview(app);

    // ── rich tooltip timer check ──────────────────────────────────
    check_hover_tooltip(app);

    // ── folder hover-to-open during drag ──────────────────────────
    if (app.drop_hover_open_start_ms > 0 && !app.drop_hover_open_path.empty()) {
      auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      if (now_ms - app.drop_hover_open_start_ms >= 800) {
        std::string path = app.drop_hover_open_path;
        app.drop_hover_open_start_ms = 0;
        app.drop_hover_open_path.clear();
        navigate_to(app, path);
        app.pendingRedraw = true;
      }
    }

    // ── tab hover-to-switch during drag ───────────────────────────
    if (app.drop_target_tab_idx >= 0 && app.drop_tab_switch_start_ms > 0 &&
        app.tabs.size() > 1) {
      auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      if (now_ms - app.drop_tab_switch_start_ms >= 600) {
        int target_tab = app.drop_target_tab_idx;
        app.drop_target_tab_idx = -1;
        app.drop_tab_switch_start_ms = 0;
        if (target_tab >= 0 && target_tab < static_cast<int>(app.tabs.size())) {
          app.active_tab = target_tab;
          reload_dir(app);
          app.pendingRedraw = true;
        }
      }
    }

    // ── application-level key repeat ───────────────────────────────
    if (app.key_repeat_sym != 0) {
      auto now = std::chrono::steady_clock::now();
      uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
      uint64_t elapsed = now_ms - app.key_repeat_start_ms;
      // 350 ms initial delay (single taps never repeat), then ~33 Hz repeat
      // for fast-but-smooth hold-to-erase / hold-to-move.
      constexpr uint64_t kRepeatDelay = 350;
      constexpr uint64_t kRepeatRate = 30;
      if (elapsed >= kRepeatDelay) {
        uint64_t delta = now_ms - app.key_repeat_last_ms;
        if (delta >= kRepeatRate) {
          int sym = app.key_repeat_sym;
          bool repeat_done = false;
          auto utf8_prev = [](const std::string& s, int pos) {
            if (pos <= 0) return 0;
            int n = static_cast<int>(s.size());
            if (pos > n) pos = n;
            --pos;
            while (pos > 0 &&
                   (static_cast<unsigned char>(s[static_cast<std::size_t>(pos)]) & 0xC0) == 0x80)
              --pos;
            return pos;
          };
          auto utf8_next = [](const std::string& s, int pos) {
            if (pos < 0) return 0;
            int n = static_cast<int>(s.size());
            if (pos >= n) return n;
            ++pos;
            while (pos < n &&
                   (static_cast<unsigned char>(s[static_cast<std::size_t>(pos)]) & 0xC0) == 0x80)
              ++pos;
            return pos;
          };
          if (app.rename_ui_open) {
            if (sym == XKB_KEY_BackSpace && !app.rename_ui_buf.empty()) {
              if (app.rename_ui_sel_start >= 0 &&
                  app.rename_ui_sel_start != app.rename_ui_sel_end) {
                int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
                int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
                if (a < 0) a = 0;
                if (b > static_cast<int>(app.rename_ui_buf.size()))
                  b = static_cast<int>(app.rename_ui_buf.size());
                app.rename_ui_buf.erase(static_cast<std::size_t>(a),
                                        static_cast<std::size_t>(b - a));
                app.rename_ui_cursor_pos = a;
                app.rename_ui_sel_start = -1;
                app.rename_ui_sel_end = -1;
              } else if (app.rename_ui_cursor_pos > 0) {
                int p = utf8_prev(app.rename_ui_buf, app.rename_ui_cursor_pos);
                app.rename_ui_buf.erase(static_cast<std::size_t>(p),
                                        static_cast<std::size_t>(app.rename_ui_cursor_pos - p));
                app.rename_ui_cursor_pos = p;
              } else {
                // At column 0: nothing to erase, stop arming instead of
                // spinning the event loop at 0 ms poll.
                app.key_repeat_sym = 0;
                repeat_done = true;
              }
              if (!repeat_done) {
                app.pendingRedraw = true;
                if (app.rename_ui_buf.empty()) {
                  app.key_repeat_sym = 0;
                  repeat_done = true;
                }
              }
            } else if (sym == XKB_KEY_Delete && !app.rename_ui_buf.empty()) {
              if (app.rename_ui_sel_start >= 0 &&
                  app.rename_ui_sel_start != app.rename_ui_sel_end) {
                int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
                int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
                if (a < 0) a = 0;
                if (b > static_cast<int>(app.rename_ui_buf.size()))
                  b = static_cast<int>(app.rename_ui_buf.size());
                app.rename_ui_buf.erase(static_cast<std::size_t>(a),
                                        static_cast<std::size_t>(b - a));
                app.rename_ui_cursor_pos = a;
                app.rename_ui_sel_start = -1;
                app.rename_ui_sel_end = -1;
              } else if (app.rename_ui_cursor_pos <
                         static_cast<int>(app.rename_ui_buf.size())) {
                int p = utf8_next(app.rename_ui_buf, app.rename_ui_cursor_pos);
                app.rename_ui_buf.erase(static_cast<std::size_t>(app.rename_ui_cursor_pos),
                                        static_cast<std::size_t>(p - app.rename_ui_cursor_pos));
              } else {
                app.key_repeat_sym = 0;
                repeat_done = true;
              }
              if (!repeat_done) {
                if (app.rename_ui_cursor_pos >
                    static_cast<int>(app.rename_ui_buf.size()))
                  app.rename_ui_cursor_pos =
                      static_cast<int>(app.rename_ui_buf.size());
                app.pendingRedraw = true;
                if (app.rename_ui_buf.empty()) {
                  app.key_repeat_sym = 0;
                  repeat_done = true;
                }
              }
            } else if (sym == XKB_KEY_Left && app.rename_ui_cursor_pos > 0) {
              app.rename_ui_cursor_pos =
                  utf8_prev(app.rename_ui_buf, app.rename_ui_cursor_pos);
              if (app.rename_ui_sel_start >= 0)
                app.rename_ui_sel_end = app.rename_ui_cursor_pos;
              app.pendingRedraw = true;
            } else if (sym == XKB_KEY_Right && app.rename_ui_cursor_pos < static_cast<int>(app.rename_ui_buf.size())) {
              app.rename_ui_cursor_pos =
                  utf8_next(app.rename_ui_buf, app.rename_ui_cursor_pos);
              if (app.rename_ui_sel_start >= 0)
                app.rename_ui_sel_end = app.rename_ui_cursor_pos;
              app.pendingRedraw = true;
            }
          }
          if (!repeat_done && app.create_dialog_open) {
            if (sym == XKB_KEY_BackSpace && !app.create_buf.empty()) {
              if (app.create_cursor_pos > 0) {
                int p = utf8_prev(app.create_buf, app.create_cursor_pos);
                app.create_buf.erase(static_cast<std::size_t>(p),
                                     static_cast<std::size_t>(app.create_cursor_pos - p));
                app.create_cursor_pos = p;
              } else {
                app.key_repeat_sym = 0;
                repeat_done = true;
              }
              if (!repeat_done) {
                app.pendingRedraw = true;
                if (app.create_buf.empty()) {
                  app.key_repeat_sym = 0;
                  repeat_done = true;
                }
              }
            }
          }
          if (app.pendingRedraw)
            app.key_repeat_last_ms = now_ms;
        }
      }
    }

    DeferredCall::drain();

    if (op_active) {
      static auto last_progress_draw = std::chrono::steady_clock::time_point{};
      auto now_tp = std::chrono::steady_clock::now();
      if (now_tp - last_progress_draw > std::chrono::milliseconds(30)) {
        last_progress_draw = now_tp;
        app.pendingRedraw = true;
      }
    }

    if (app.pendingRedraw) {
      app.pendingRedraw = false;
      if (app.surface) {
        if (app.resize_buffers_dirty && app.shm) {
          // Paced resize paint: at most one per loop iteration, timed.
          const bool tr =
              eh::trace::enabled().load(std::memory_order_relaxed);
          auto tick_t0 = std::chrono::steady_clock::now();
          auto buf_t0 = tick_t0;
          app.buf[0].ensure(app.shm, "eh-fb-a", app.width, app.height);
          app.buf[1].ensure(app.shm, "eh-fb-b", app.width, app.height);
          double buf_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - buf_t0)
                              .count();
          // Buffers now match the configured size — clear BEFORE drawing so
          // draw()'s stale-buffer guard lets this paint through.
          app.resize_buffers_dirty = false;
          app.last_paint_w = app.width;
          app.last_paint_h = app.height;
          auto draw_t0 = std::chrono::steady_clock::now();
          draw(app);
          double draw_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - draw_t0)
                               .count();
          double tick_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - tick_t0)
                               .count();
          if (tr) {
            trace::log("RESIZE TICK %dx%d buf %.2f ms | draw %.2f ms | "
                       "tick %.2f ms",
                       app.width, app.height, buf_ms, draw_ms, tick_ms);
            if (!app.resize_session_active) {
              app.resize_session_active = true;
              app.resize_ticks = 0;
              app.resize_drops = 0;
              app.resize_tick_ms_sum = 0.0;
              app.resize_tick_ms_max = 0.0;
              app.resize_buf_ms_max = 0.0;
              app.resize_draw_ms_max = 0.0;
              app.resize_phase_samples.clear();
              trace::log("RESIZE SESSION begin");
            }
            ++app.resize_ticks;
            app.resize_tick_ms_sum += tick_ms;
            app.resize_tick_ms_max =
                std::max(app.resize_tick_ms_max, tick_ms);
            app.resize_buf_ms_max =
                std::max(app.resize_buf_ms_max, buf_ms);
            app.resize_draw_ms_max =
                std::max(app.resize_draw_ms_max, draw_ms);
          }
        } else {
          draw(app);
        }
      }
    }

    if (app.props_pendingRedraw) {
      app.props_pendingRedraw = false;
      if (app.props_surface) draw_props_window(app);
    }

    if (app.settings_pendingRedraw) {
      app.settings_pendingRedraw = false;
      if (app.settings_surface) draw_settings_window(app);
    }

    // Resize session wound down: dump the perf map once things settle.
    if (app.resize_session_active &&
        std::chrono::steady_clock::now() - app.resize_last_size_change >
            std::chrono::milliseconds(250)) {
      resize_session_dump(app);
    }
  }

  // Persist per-directory view/zoom memory before teardown so each folder's
  // settings survive a restart (clean quit AND compositor-driven close).
  save_file_browser_settings(app);

  // Cleanup (destroy dialog windows first, then shm-buffers BEFORE disconnecting the display)
  join_scan(app);  // reap background directory scanner before teardown
  eh::file_browser::thumb_pool_stop();
  eh::file_browser::dir_stats_stop();
  eh::file_browser::dir_watch_close(app);
  destroy_settings_window(app);
  destroy_props_window(app);
  for (auto& b : app.buf) b.destroy();
  app.previewPopupBuf.destroy();
  if (app.toplevel) xdg_toplevel_destroy(app.toplevel);
  if (app.xdgSurface) xdg_surface_destroy(app.xdgSurface);
  if (app.surface) wl_surface_destroy(app.surface);
  app.clipboard.cleanup();
  app.seat.unbind();
  app.wl.disconnect();

  g_app = nullptr;
  return 0;
}

// ── run select directory ─────────────────────────────────────────

[[nodiscard]] int run_select_directory(std::string& out_path) {
  g_app = std::make_unique<AppState>();
  AppState& app = *g_app;
  app.select_dir_mode = true;

  if (!app.wl.connect()) {
    std::cerr << "WAYLAND_DISPLAY not set or compositor unavailable.\n";
    return 1;
  }

  if (!connect_globals(app)) {
    std::cerr << "Failed to connect Wayland globals.\n";
    return 1;
  }

  if (!create_window(app)) {
    std::cerr << "Failed to create file browser window.\n";
    return 1;
  }

  // Set title for picker mode
  if (app.toplevel)
    xdg_toplevel_set_title(app.toplevel, "Select Directory");

  // Load arrow-icon SVGs
  static constexpr int kArrowIconLoadPx = 256;
  app.arrow_left_svg  = eh::shell::asset::load_asset_svg("UI", "arrow-left.svg", kArrowIconLoadPx);
  app.arrow_right_svg = eh::shell::asset::load_asset_svg("UI", "arrow-right.svg", kArrowIconLoadPx);
  app.arrow_up_svg    = eh::shell::asset::load_asset_svg("UI", "arrow-up.svg", kArrowIconLoadPx);
  app.search_svg      = eh::shell::asset::load_asset_svg("UI", "search.svg", kArrowIconLoadPx);
  app.folder_search_svg = eh::shell::asset::load_asset_svg("UI", "folder-search.svg", kArrowIconLoadPx);
  app.view_list_svg     = eh::shell::asset::load_asset_svg("UI", "view-list.svg", kArrowIconLoadPx);
  app.view_grid_svg     = eh::shell::asset::load_asset_svg("UI", "view-grid.svg", kArrowIconLoadPx);
  app.view_compact_svg  = eh::shell::asset::load_asset_svg("UI", "view-compact.svg", kArrowIconLoadPx);
  app.view_tree_svg     = eh::shell::asset::load_asset_svg("UI", "view-tree.svg", kArrowIconLoadPx);
  app.settings_gear_svg = eh::shell::asset::load_asset_svg("UI", "settings-gear.svg", kArrowIconLoadPx);
  app.three_dots_svg    = eh::shell::asset::load_asset_svg("UI", "three-dots.svg", kArrowIconLoadPx);
  app.sidebar_toggle_svg = eh::shell::asset::load_asset_svg("UI", "layout.svg", kArrowIconLoadPx);
  app.sort_chevron_svg  = eh::shell::asset::load_asset_svg("UI", "chevron-down.svg", kArrowIconLoadPx);
  app.checkmark_svg     = eh::shell::asset::load_asset_svg("UI", "checkmark.svg", kArrowIconLoadPx);
  app.arrow_down_svg    = eh::shell::asset::load_asset_svg("UI", "arrow-down.svg", kArrowIconLoadPx);
  app.arrow_downward_svg= eh::shell::asset::load_asset_svg("UI", "arrow-downward.svg", kArrowIconLoadPx);
  app.icon_hash_svg     = eh::shell::asset::load_asset_svg("UI", "hash.svg", kArrowIconLoadPx);
  app.icon_bars_svg     = eh::shell::asset::load_asset_svg("UI", "bar-chart-2.svg", kArrowIconLoadPx);
  app.icon_clock_svg    = eh::shell::asset::load_asset_svg("UI", "clock.svg", kArrowIconLoadPx);
  app.icon_file_text_svg= eh::shell::asset::load_asset_svg("UI", "file-text.svg", kArrowIconLoadPx);
  app.icon_person_svg   = eh::shell::asset::load_asset_svg("UI", "person.svg", kArrowIconLoadPx);
  app.icon_people_svg   = eh::shell::asset::load_asset_svg("UI", "people.svg", kArrowIconLoadPx);
  app.icon_shield_svg   = eh::shell::asset::load_asset_svg("UI", "shield.svg", kArrowIconLoadPx);
  app.icon_file_svg     = eh::shell::asset::load_asset_svg("UI", "file.svg", kArrowIconLoadPx);
  app.icon_link_svg     = eh::shell::asset::load_asset_svg("UI", "link.svg", kArrowIconLoadPx);
  app.icon_folder_svg   = eh::shell::asset::load_asset_svg("UI", "folder.svg", kArrowIconLoadPx);
  app.icon_eyeoff_svg   = eh::shell::asset::load_asset_svg("UI", "eye-off.svg", kArrowIconLoadPx);
  app.icon_list_svg     = eh::shell::asset::load_asset_svg("UI", "list.svg", kArrowIconLoadPx);
  app.icon_aa_svg       = eh::shell::asset::load_asset_svg("UI", "text.svg", kArrowIconLoadPx);
  app.icon_minus_svg    = eh::shell::asset::load_asset_svg("UI", "minus.svg", kArrowIconLoadPx);
  app.edit_svg          = eh::shell::asset::load_asset_svg("UI", "edit-2.svg", kArrowIconLoadPx);
  app.lock_svg          = eh::shell::asset::load_asset_svg("UI", "lock.svg", kArrowIconLoadPx);
  app.trash_svg         = eh::shell::asset::load_asset_svg("UI", "trash.svg", kArrowIconLoadPx);
  app.monitor_svg       = eh::shell::asset::load_asset_svg("UI", "monitor.svg", kArrowIconLoadPx);
  app.home_nav_svg      = eh::shell::asset::load_asset_svg("UI", "home.svg", kArrowIconLoadPx);
  app.music_nav_svg     = eh::shell::asset::load_asset_svg("UI", "music.svg", kArrowIconLoadPx);
  app.video_nav_svg     = eh::shell::asset::load_asset_svg("UI", "video.svg", kArrowIconLoadPx);
  app.documents_nav_svg = eh::shell::asset::load_asset_svg("UI", "documents.svg", kArrowIconLoadPx);
  app.mounted_svg     = eh::shell::asset::load_asset_svg("UI", "Mounted.svg", kArrowIconLoadPx);
  app.icon_desktop_svg    = eh::shell::asset::load_asset_svg("UI", "icon-desktop.svg", 64);
  app.icon_documents_svg  = eh::shell::asset::load_asset_svg("UI", "icon-documents.svg", 64);
  app.icon_downloads_svg  = eh::shell::asset::load_asset_svg("UI", "icon-downloads.svg", 64);
  app.icon_music_svg      = eh::shell::asset::load_asset_svg("UI", "icon-music.svg", 64);
  app.icon_pictures_svg   = eh::shell::asset::load_asset_svg("UI", "icon-pictures.svg", 64);
  app.icon_videos_svg     = eh::shell::asset::load_asset_svg("UI", "icon-videos.svg", 64);
  app.icon_publicshare_svg  = eh::shell::asset::load_asset_svg("UI", "icon-publicshare.svg", 64);
  app.icon_templates_svg    = eh::shell::asset::load_asset_svg("UI", "icon-templates.svg", 64);
  if (!app.arrow_left_svg || !app.arrow_right_svg || !app.arrow_up_svg || !app.search_svg || !app.folder_search_svg) {
    std::fprintf(stderr, "[horizon-files] WARNING: arrow/search SVGs not loaded (L=%p R=%p U=%p S=%p FS=%p). "
                         "CWD=%s\n",
                 (void*)app.arrow_left_svg, (void*)app.arrow_right_svg, (void*)app.arrow_up_svg,
                 (void*)app.search_svg, (void*)app.folder_search_svg,
                 []{ static char buf[4096]; return getcwd(buf, sizeof(buf)) ? buf : "?"; }());
  }

  // Initialize icon theme
  {
    const auto& sc = eh::config::shell_config_snapshot_skip_matugen();
    if (!sc.dock.iconTheme.empty()) {
      app.icons.set_icon_theme(sc.dock.iconTheme);
      app.last_icon_theme = sc.dock.iconTheme;
    }
    app.icons.prewarm_search_dirs();
  }

  reload_settings_from_config(app);

  // Initialize
  refresh_sidebar(app);
  reload_dir(app);

  // Wire up input event callbacks
  if (app.seat.pointer()) {
    app.seat.set_pointer_motion_cb(
        [&app](wl_surface* surface, double x, double y) {
          app.focused_surface = surface;
          if (surface == app.settings_surface) {
            app.settings_pointerX = static_cast<int>(x);
            app.settings_pointerY = static_cast<int>(y);
            if (app.settings_slider_dragging != 0) {
              settings_apply_slider(app, app.settings_slider_dragging,
                                    static_cast<int>(x));
              app.settings_pendingRedraw = true;
              app.pendingRedraw = true;
            } else {
              app.settings_pendingRedraw = true;
            }
          } else if (surface == app.props_surface) {
            app.props_pointerX = static_cast<int>(x);
            app.props_pointerY = static_cast<int>(y);
            app.props_pendingRedraw = true;
          } else {
            app.pointerX = x;
            app.pointerY = y;
            handle_pointer_move(app, static_cast<int>(x), static_cast<int>(y));
          }
        });
    app.seat.set_pointer_button_cb(
        [&app](uint32_t button, uint32_t state) {
          if (app.focused_surface == app.settings_surface) {
            if (state == 1) {
              handle_settings_click(app, app.settings_pointerX,
                                    app.settings_pointerY,
                                    static_cast<int>(button));
            } else {
              app.settings_slider_dragging = 0;
            }
            return;
          }
          if (app.focused_surface == app.props_surface) {
            if (state == 1) {
              handle_props_click(app, app.props_pointerX,
                                 app.props_pointerY,
                                 static_cast<int>(button));
            }
            return;
          }
          if (state == 1) {
            handle_click(app, static_cast<int>(app.pointerX),
                         static_cast<int>(app.pointerY),
                         static_cast<int>(button));
          } else {
            handle_pointer_release(app, static_cast<int>(app.pointerX),
                                   static_cast<int>(app.pointerY),
                                   static_cast<int>(button));
          }
        });
    app.seat.set_pointer_axis_vertical_cb(
        [&app](double delta_px) {
          if (app.focused_surface == app.settings_surface) {
            return;
          }
          if (app.focused_surface == app.props_surface) {
            app.properties.scroll_px += static_cast<int>(-delta_px);
            if (app.properties.scroll_px < 0) app.properties.scroll_px = 0;
            app.props_pendingRedraw = true;
            return;
          }
          handle_scroll(app, static_cast<int>(app.pointerX),
                        static_cast<int>(app.pointerY), 0.0, delta_px);
        });
  }
  if (app.seat.keyboard()) {
    app.seat.set_keyboard_key_cb(
        [&app](const eh::wayland::WaylandSeat::KeyboardEvent& ev) {
          int len = std::min(ev.utf8_len, static_cast<int>(ev.utf8.size()) - 1);
          handle_key(app, ev.keycode, ev.state, ev.sym, ev.utf8.data(), len);
        });
  }

  // Signal handling
  g_signal = 0;
  {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
  }

  // Initial draw
  draw(app);

  // Event loop
  const int dpy_fd = wl_display_get_fd(app.wl.display());
  constexpr int kPollMs = 200;

  eh::file_browser::thumb_pool_start(&app);
  eh::file_browser::dir_stats_start();
  warmup_text_rendering();
  while (app.running && g_signal == 0) {
    struct pollfd pf{};
    pf.fd = dpy_fd;
    pf.events = POLLIN | POLLERR | POLLHUP;

    int pr = poll(&pf, 1, kPollMs);
    if (pr < 0) {
      if (errno == EINTR) {
        if (g_signal != 0) break;
        continue;
      }
      break;
    }
    if (g_signal != 0) break;
    if (pf.revents & (POLLERR | POLLHUP)) break;

    if (pf.revents & POLLIN) {
      auto disp_t0 = std::chrono::steady_clock::now();
      uint64_t ev0 = eh::wayland::WaylandSeat::pointer_event_count();
      if (wl_display_dispatch(app.wl.display()) < 0) break;
      double disp_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - disp_t0)
                           .count();
      // Click handling runs inside dispatch, so long stalls here directly
      // hurt input latency — make them visible.
      if (disp_ms >= 100.0 && trace::enabled().load(std::memory_order_relaxed))
        trace::log("DISPATCH SLOW %.1f ms events=%llu", disp_ms,
                   (unsigned long long)(eh::wayland::WaylandSeat::pointer_event_count() -
                                        ev0));
    } else {
      wl_display_flush(app.wl.display());
    }

    if (app.pendingRedraw) {
      app.pendingRedraw = false;
      if (app.surface) draw(app);
    }

    if (app.props_pendingRedraw) {
      app.props_pendingRedraw = false;
      if (app.props_surface) draw_props_window(app);
    }
  }

  // Collect result
  if (!app.select_dir_result.empty()) {
    out_path = std::move(app.select_dir_result);
  }

  // Cleanup (destroy props window first, then shm-buffers BEFORE disconnecting the display)
  destroy_props_window(app);
  for (auto& b : app.buf) b.destroy();
  app.previewPopupBuf.destroy();
  if (app.toplevel) xdg_toplevel_destroy(app.toplevel);
  if (app.xdgSurface) xdg_surface_destroy(app.xdgSurface);
  if (app.surface) wl_surface_destroy(app.surface);
  app.clipboard.cleanup();
  app.seat.unbind();
  app.wl.disconnect();

  g_app = nullptr;
  return out_path.empty() ? 1 : 0;
}

[[nodiscard]] int run_select_file(std::string& out_path) {
  g_app = std::make_unique<AppState>();
  AppState& app = *g_app;
  app.select_file_mode = true;

  if (!app.wl.connect()) {
    std::cerr << "WAYLAND_DISPLAY not set or compositor unavailable.\n";
    return 1;
  }

  if (!connect_globals(app)) {
    std::cerr << "Failed to connect Wayland globals.\n";
    return 1;
  }

  if (!create_window(app)) {
    std::cerr << "Failed to create file browser window.\n";
    return 1;
  }

  // Set title for picker mode
  if (app.toplevel)
    xdg_toplevel_set_title(app.toplevel, "Select File");

  // Load arrow-icon SVGs
  static constexpr int kArrowIconLoadPx = 256;
  app.arrow_left_svg  = eh::shell::asset::load_asset_svg("UI", "arrow-left.svg", kArrowIconLoadPx);
  app.arrow_right_svg = eh::shell::asset::load_asset_svg("UI", "arrow-right.svg", kArrowIconLoadPx);
  app.arrow_up_svg    = eh::shell::asset::load_asset_svg("UI", "arrow-up.svg", kArrowIconLoadPx);
  app.search_svg      = eh::shell::asset::load_asset_svg("UI", "search.svg", kArrowIconLoadPx);
  app.folder_search_svg = eh::shell::asset::load_asset_svg("UI", "folder-search.svg", kArrowIconLoadPx);
  app.view_list_svg     = eh::shell::asset::load_asset_svg("UI", "view-list.svg", kArrowIconLoadPx);
  app.view_grid_svg     = eh::shell::asset::load_asset_svg("UI", "view-grid.svg", kArrowIconLoadPx);
  app.view_compact_svg  = eh::shell::asset::load_asset_svg("UI", "view-compact.svg", kArrowIconLoadPx);
  app.view_tree_svg     = eh::shell::asset::load_asset_svg("UI", "view-tree.svg", kArrowIconLoadPx);
  app.settings_gear_svg = eh::shell::asset::load_asset_svg("UI", "settings-gear.svg", kArrowIconLoadPx);
  app.three_dots_svg    = eh::shell::asset::load_asset_svg("UI", "three-dots.svg", kArrowIconLoadPx);
  app.sidebar_toggle_svg = eh::shell::asset::load_asset_svg("UI", "layout.svg", kArrowIconLoadPx);
  app.sort_chevron_svg  = eh::shell::asset::load_asset_svg("UI", "chevron-down.svg", kArrowIconLoadPx);
  app.checkmark_svg     = eh::shell::asset::load_asset_svg("UI", "checkmark.svg", kArrowIconLoadPx);
  app.arrow_down_svg    = eh::shell::asset::load_asset_svg("UI", "arrow-down.svg", kArrowIconLoadPx);
  app.arrow_downward_svg= eh::shell::asset::load_asset_svg("UI", "arrow-downward.svg", kArrowIconLoadPx);
  app.icon_hash_svg     = eh::shell::asset::load_asset_svg("UI", "hash.svg", kArrowIconLoadPx);
  app.icon_bars_svg     = eh::shell::asset::load_asset_svg("UI", "bar-chart-2.svg", kArrowIconLoadPx);
  app.icon_clock_svg    = eh::shell::asset::load_asset_svg("UI", "clock.svg", kArrowIconLoadPx);
  app.icon_file_text_svg= eh::shell::asset::load_asset_svg("UI", "file-text.svg", kArrowIconLoadPx);
  app.icon_person_svg   = eh::shell::asset::load_asset_svg("UI", "person.svg", kArrowIconLoadPx);
  app.icon_people_svg   = eh::shell::asset::load_asset_svg("UI", "people.svg", kArrowIconLoadPx);
  app.icon_shield_svg   = eh::shell::asset::load_asset_svg("UI", "shield.svg", kArrowIconLoadPx);
  app.icon_file_svg     = eh::shell::asset::load_asset_svg("UI", "file.svg", kArrowIconLoadPx);
  app.icon_link_svg     = eh::shell::asset::load_asset_svg("UI", "link.svg", kArrowIconLoadPx);
  app.icon_folder_svg   = eh::shell::asset::load_asset_svg("UI", "folder.svg", kArrowIconLoadPx);
  app.icon_eyeoff_svg   = eh::shell::asset::load_asset_svg("UI", "eye-off.svg", kArrowIconLoadPx);
  app.icon_list_svg     = eh::shell::asset::load_asset_svg("UI", "list.svg", kArrowIconLoadPx);
  app.icon_aa_svg       = eh::shell::asset::load_asset_svg("UI", "text.svg", kArrowIconLoadPx);
  app.icon_minus_svg    = eh::shell::asset::load_asset_svg("UI", "minus.svg", kArrowIconLoadPx);
  app.edit_svg          = eh::shell::asset::load_asset_svg("UI", "edit-2.svg", kArrowIconLoadPx);
  app.lock_svg          = eh::shell::asset::load_asset_svg("UI", "lock.svg", kArrowIconLoadPx);
  app.trash_svg         = eh::shell::asset::load_asset_svg("UI", "trash.svg", kArrowIconLoadPx);
  app.monitor_svg       = eh::shell::asset::load_asset_svg("UI", "monitor.svg", kArrowIconLoadPx);
  app.home_nav_svg      = eh::shell::asset::load_asset_svg("UI", "home.svg", kArrowIconLoadPx);
  app.music_nav_svg     = eh::shell::asset::load_asset_svg("UI", "music.svg", kArrowIconLoadPx);
  app.video_nav_svg     = eh::shell::asset::load_asset_svg("UI", "video.svg", kArrowIconLoadPx);
  app.documents_nav_svg = eh::shell::asset::load_asset_svg("UI", "documents.svg", kArrowIconLoadPx);
  app.mounted_svg     = eh::shell::asset::load_asset_svg("UI", "Mounted.svg", kArrowIconLoadPx);
  app.icon_desktop_svg    = eh::shell::asset::load_asset_svg("UI", "icon-desktop.svg", 64);
  app.icon_documents_svg  = eh::shell::asset::load_asset_svg("UI", "icon-documents.svg", 64);
  app.icon_downloads_svg  = eh::shell::asset::load_asset_svg("UI", "icon-downloads.svg", 64);
  app.icon_music_svg      = eh::shell::asset::load_asset_svg("UI", "icon-music.svg", 64);
  app.icon_pictures_svg   = eh::shell::asset::load_asset_svg("UI", "icon-pictures.svg", 64);
  app.icon_videos_svg     = eh::shell::asset::load_asset_svg("UI", "icon-videos.svg", 64);
  app.icon_publicshare_svg  = eh::shell::asset::load_asset_svg("UI", "icon-publicshare.svg", 64);
  app.icon_templates_svg    = eh::shell::asset::load_asset_svg("UI", "icon-templates.svg", 64);
  if (!app.arrow_left_svg || !app.arrow_right_svg || !app.arrow_up_svg || !app.search_svg || !app.folder_search_svg) {
    std::fprintf(stderr, "[horizon-files] WARNING: arrow/search SVGs not loaded (L=%p R=%p U=%p S=%p FS=%p). "
                         "CWD=%s\n",
                 (void*)app.arrow_left_svg, (void*)app.arrow_right_svg, (void*)app.arrow_up_svg,
                 (void*)app.search_svg, (void*)app.folder_search_svg,
                 []{ static char buf[4096]; return getcwd(buf, sizeof(buf)) ? buf : "?"; }());
  }

  // Initialize icon theme
  {
    const auto& sc = eh::config::shell_config_snapshot_skip_matugen();
    if (!sc.dock.iconTheme.empty()) {
      app.icons.set_icon_theme(sc.dock.iconTheme);
      app.last_icon_theme = sc.dock.iconTheme;
    }
    app.icons.prewarm_search_dirs();
  }

  reload_settings_from_config(app);

  // Initialize
  refresh_sidebar(app);
  reload_dir(app);

  // Wire up input event callbacks
  if (app.seat.pointer()) {
    app.seat.set_pointer_motion_cb(
        [&app](wl_surface* surface, double x, double y) {
          app.focused_surface = surface;
          if (surface == app.settings_surface) {
            app.settings_pointerX = static_cast<int>(x);
            app.settings_pointerY = static_cast<int>(y);
            if (app.settings_slider_dragging != 0) {
              settings_apply_slider(app, app.settings_slider_dragging,
                                    static_cast<int>(x));
              app.settings_pendingRedraw = true;
              app.pendingRedraw = true;
            } else {
              app.settings_pendingRedraw = true;
            }
          } else if (surface == app.props_surface) {
            app.props_pointerX = static_cast<int>(x);
            app.props_pointerY = static_cast<int>(y);
            app.props_pendingRedraw = true;
          } else {
            app.pointerX = x;
            app.pointerY = y;
            handle_pointer_move(app, static_cast<int>(x), static_cast<int>(y));
          }
        });
    app.seat.set_pointer_button_cb(
        [&app](uint32_t button, uint32_t state) {
          if (app.focused_surface == app.settings_surface) {
            if (state == 1) {
              handle_settings_click(app, app.settings_pointerX,
                                    app.settings_pointerY,
                                    static_cast<int>(button));
            } else {
              app.settings_slider_dragging = 0;
            }
            return;
          }
          if (app.focused_surface == app.props_surface) {
            if (state == 1) {
              handle_props_click(app, app.props_pointerX,
                                 app.props_pointerY,
                                 static_cast<int>(button));
            }
            return;
          }
          if (state == 1) {
            handle_click(app, static_cast<int>(app.pointerX),
                         static_cast<int>(app.pointerY),
                         static_cast<int>(button));
          } else {
            handle_pointer_release(app, static_cast<int>(app.pointerX),
                                   static_cast<int>(app.pointerY),
                                   static_cast<int>(button));
          }
        });
    app.seat.set_pointer_axis_vertical_cb(
        [&app](double delta_px) {
          if (app.focused_surface == app.settings_surface) {
            return;
          }
          if (app.focused_surface == app.props_surface) {
            app.properties.scroll_px += static_cast<int>(-delta_px);
            if (app.properties.scroll_px < 0) app.properties.scroll_px = 0;
            app.props_pendingRedraw = true;
            return;
          }
          handle_scroll(app, static_cast<int>(app.pointerX),
                        static_cast<int>(app.pointerY), 0.0, delta_px);
        });
  }
  if (app.seat.keyboard()) {
    app.seat.set_keyboard_key_cb(
        [&app](const eh::wayland::WaylandSeat::KeyboardEvent& ev) {
          int len = std::min(ev.utf8_len, static_cast<int>(ev.utf8.size()) - 1);
          handle_key(app, ev.keycode, ev.state, ev.sym, ev.utf8.data(), len);
        });
  }

  // Signal handling
  g_signal = 0;
  {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
  }

  // Initial draw
  draw(app);

  // Event loop
  const int dpy_fd = wl_display_get_fd(app.wl.display());
  constexpr int kPollMs = 200;

  eh::file_browser::thumb_pool_start(&app);
  eh::file_browser::dir_stats_start();
  warmup_text_rendering();
  while (app.running && g_signal == 0) {
    struct pollfd pf{};
    pf.fd = dpy_fd;
    pf.events = POLLIN | POLLERR | POLLHUP;

    int pr = poll(&pf, 1, kPollMs);
    if (pr < 0) {
      if (errno == EINTR) {
        if (g_signal != 0) break;
        continue;
      }
      break;
    }
    if (g_signal != 0) break;
    if (pf.revents & (POLLERR | POLLHUP)) break;

    if (pf.revents & POLLIN) {
      auto disp_t0 = std::chrono::steady_clock::now();
      uint64_t ev0 = eh::wayland::WaylandSeat::pointer_event_count();
      if (wl_display_dispatch(app.wl.display()) < 0) break;
      double disp_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - disp_t0)
                           .count();
      // Click handling runs inside dispatch, so long stalls here directly
      // hurt input latency — make them visible.
      if (disp_ms >= 100.0 && trace::enabled().load(std::memory_order_relaxed))
        trace::log("DISPATCH SLOW %.1f ms events=%llu", disp_ms,
                   (unsigned long long)(eh::wayland::WaylandSeat::pointer_event_count() -
                                        ev0));
    } else {
      wl_display_flush(app.wl.display());
    }

    if (app.pendingRedraw) {
      app.pendingRedraw = false;
      if (app.surface) draw(app);
    }

    if (app.props_pendingRedraw) {
      app.props_pendingRedraw = false;
      if (app.props_surface) draw_props_window(app);
    }
  }

  // Collect result
  if (!app.select_dir_result.empty()) {
    out_path = std::move(app.select_dir_result);
  }

  // Cleanup (destroy props window first, then shm-buffers BEFORE disconnecting the display)
  destroy_props_window(app);
  for (auto& b : app.buf) b.destroy();
  app.previewPopupBuf.destroy();
  if (app.toplevel) xdg_toplevel_destroy(app.toplevel);
  if (app.xdgSurface) xdg_surface_destroy(app.xdgSurface);
  if (app.surface) wl_surface_destroy(app.surface);
  app.clipboard.cleanup();
  app.seat.unbind();
  app.wl.disconnect();

  g_app = nullptr;
  return out_path.empty() ? 1 : 0;
}

} // namespace eh::file_browser
