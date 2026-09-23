// scroll.cpp — mouse-wheel / touchpad scroll handler. Moved from
// events.cpp (byte-identical).
#include "events.hpp"
#include "../app.hpp"
#include "../features/compress/compress.hpp"
#include "../features/drag/drag.hpp"
#include "../features/progress/progress.hpp"
#include "../features/query_match/query_match.hpp"
#include "../features/recursive_search_worker/recursive_search_worker.hpp"
#include "../features/selection/selection.hpp"
#include "../features/sidebar/sidebar.hpp"
#include "../features/tab_history/tab_history.hpp"
#include "../features/tags/tags.hpp"
#include "../features/view_zoom/view_zoom.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <pwd.h>
#include <sys/stat.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "config/shell_config.hpp"
#include "platform/desktop/entries/desktop_xdg_ops.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;

namespace eh::file_browser {
// ── scroll handler (moved from events.cpp) ──────────────────────────
void handle_scroll(AppState& app, int x, int, double, double dy) {
  // ── Sort menu wheel scroll ──
  if (app.r_sort_menu_open || app.sort_menu_open) {
    auto& smx = app.active_pane ? app.r_sort_menu_x : app.sort_menu_x;
    auto& smy = app.active_pane ? app.r_sort_menu_y : app.sort_menu_y;
    auto& smw = app.active_pane ? app.r_sort_menu_w : app.sort_menu_w;
    auto& smh = app.active_pane ? app.r_sort_menu_h : app.sort_menu_h;
    if (x >= smx && x < smx + smw && app.pointerY >= smy && app.pointerY < smy + smh) {
      int& sc = app.active_pane ? app.r_sort_menu_scroll : app.sort_menu_scroll;
      int n = sort_menu_row_count();
      int visible = std::clamp((smh - kSortMenuPad * 2) / kSortMenuItemH, 1, n);
      int max_scroll = std::max(0, n - visible);
      int ns = std::clamp(sc + (dy > 0 ? 1 : -1), 0, max_scroll);
      if (ns != sc) {
        sc = ns;
        draw(app);
      }
      return;
    }
  }

  if (app.open_with_open) {
    int total = static_cast<int>(app.open_with_apps.size());
    int rec_count = app.open_with_exact_count;
    int entry_h = 40, section_h = 26;
    int total_content_h = total * entry_h;
    if (rec_count > 0) total_content_h += section_h;
    if (rec_count < total) total_content_h += section_h;
    int list_h = std::min(total_content_h, 320);
    int max_scroll = std::max(0, total_content_h - list_h);
    if (max_scroll > 0) {
      int delta = static_cast<int>(-dy * 1.5);
      int new_scroll = std::clamp(app.open_with_scroll + delta, 0, max_scroll);
      if (new_scroll != app.open_with_scroll) {
        app.open_with_scroll = new_scroll;
        draw(app);
      }
    }
    return;
  }

  // ── Properties scroll ──
  if (app.properties.open) {
    int max_scroll = std::max(0, app.properties.content_h - (static_cast<int>(app.properties.h) - 80));
    int delta = (dy > 0) ? 20 : -20;
    int new_scroll = std::clamp(app.properties.scroll_px + delta, 0, max_scroll);
    if (new_scroll != app.properties.scroll_px) {
      app.properties.scroll_px = new_scroll;
      draw(app);
    }
    return;
  }

  // ── Wheel over the zoom control: one discrete level per tick ──
  if (app.status_zoom_slider_w > 0 &&
      app.pointerY >= app.height - app.status_bar_height &&
      x >= app.status_zoom_slider_x - 40 &&
      x < app.status_zoom_slider_x + app.status_zoom_slider_w + 40) {
    step_zoom(app, dy > 0 ? +1 : -1);
    save_file_browser_settings(app);
    draw(app);
    return;
  }

  if (x < app.effective_sidebar_width()) {
    int panel_h = (app.op_progress && app.op_progress->active) ? 100 : 0;
    int available = app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height - panel_h;
    int max_scroll = std::max(0, app.sidebar_content_h - available);
    app.sidebar_scroll_px = std::clamp(app.sidebar_scroll_px - static_cast<int>(dy * 1.2), 0, max_scroll);
    draw(app);
    return;
  }

  bool was_settled;
  if (app.cur_tab().view_mode == ViewMode::Computer) {
    int max_h = std::max(0, app.computer_content_h - (app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height));
    was_settled = std::abs(app.computer_scroll_smooth_current - app.computer_scroll_smooth_target) <= 0.5;
    int target = std::clamp(static_cast<int>(std::lround(app.computer_scroll_smooth_target)) - static_cast<int>(dy * 1.5), 0, max_h);
    app.computer_scroll_smooth_target = static_cast<double>(target);
    if (was_settled) app.computer_scroll_smooth_current = static_cast<double>(app.computer_scroll_px);
  } else {
    int max_h = std::max(0, app.cur_tab().content_h - (app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height));
    was_settled = std::abs(app.cur_tab().scroll_smooth_current - app.cur_tab().scroll_smooth_target) <= 0.5;
    int target = std::clamp(static_cast<int>(std::lround(app.cur_tab().scroll_smooth_target)) - static_cast<int>(dy * 1.5), 0, max_h);
    app.cur_tab().scroll_smooth_target = static_cast<double>(target);
    if (was_settled) app.cur_tab().scroll_smooth_current = static_cast<double>(app.cur_tab().scroll_px);
  }

  // Restart the ease-curve clock on every wheel tick so the decay factor is
  // always computed relative to *this* tick's starting position — but never
  // touch scroll_smooth_current here unless we were fully at rest, so an
  // in-flight glide keeps its current position/velocity instead of being
  // snapped back to the last settled pixel. This is what lets rapid wheel
  // ticks compound into one continuous glide instead of stuttering.
  {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    app.scroll_anim_start_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                                static_cast<uint64_t>(ts.tv_nsec);
  }
  app.scroll_needs_redraw = true;
  draw(app);
}


} // namespace eh::file_browser
