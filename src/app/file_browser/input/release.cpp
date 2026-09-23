// release.cpp — pointer-release handler. Moved from events.cpp
// (byte-identical).
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
// ── pointer-release handler (moved from events.cpp) ──────────────────────────
void handle_pointer_release(AppState& app, int x, int y, int button) {
  (void)x;
  (void)y;
  (void)button;
  if (app.scrollbar_dragging) {
    app.scrollbar_dragging = false;
    draw(app);
    return;
  }
  if (app.split_divider_dragging) {
    app.split_divider_dragging = false;
    return;
  }
  if (app.status_zoom_dragging) {
    app.status_zoom_dragging = false;
    app.status_zoom_last_level = -1;
    save_file_browser_settings(app);
    draw(app);
    return;
  }
  if (app.col_resizing >= 0) {
    app.col_resizing = -1;
    draw(app);
    return;
  }
  if (button == 0x110 && app.marquee_active) {
    // Finalize marquee selection
    app.marquee_active = false;
    if (app.cur_tab().multi_selected.empty()) {
      app.cur_tab().selected_idx = -1;
    }
    draw(app);
    return;
  }
  if ((app.active_pane ? app.r_path_editing : app.path_editing) && (app.active_pane ? app.r_path_edit_dragging : app.path_edit_dragging)) {
    auto& rel_buf = app.active_pane ? app.r_path_edit_buf : app.path_edit_buf;
    auto& rel_cursor = app.active_pane ? app.r_path_edit_cursor : app.path_edit_cursor;
    auto& rel_sel_start = app.active_pane ? app.r_path_edit_sel_start : app.path_edit_sel_start;
    auto& rel_sel_end = app.active_pane ? app.r_path_edit_sel_end : app.path_edit_sel_end;
    auto& rel_dragging = app.active_pane ? app.r_path_edit_dragging : app.path_edit_dragging;
    rel_dragging = false;
    if (rel_sel_start >= 0 &&
        rel_sel_start != rel_sel_end) {
      int sel_a = std::min(rel_sel_start, rel_sel_end);
      int sel_b = std::max(rel_sel_start, rel_sel_end);
      std::string sel = rel_buf.substr(sel_a, sel_b - sel_a);
      if (!sel.empty()) app.clipboard.copy_text(sel);
    }
  }
  // Stop dialog input field drag selection
  if (app.create_dragging) {
    app.create_dragging = false;
    if (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end) {
      int a = std::min(app.create_sel_start, app.create_sel_end);
      int b = std::max(app.create_sel_start, app.create_sel_end);
      std::string sel = app.create_buf.substr(a, b - a);
      if (!sel.empty()) app.clipboard.copy_text(sel);
    }
  }
  if (app.rename_ui_dragging) {
    app.rename_ui_dragging = false;
    if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
      int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
      int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
      std::string sel = app.rename_ui_buf.substr(a, b - a);
      if (!sel.empty()) app.clipboard.copy_text(sel);
    }
  }
  if (app.password_dragging) {
    app.password_dragging = false;
    if (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end) {
      int a = std::min(app.password_sel_start, app.password_sel_end);
      int b = std::max(app.password_sel_start, app.password_sel_end);
      std::string sel = app.password_buf.substr(a, b - a);
      if (!sel.empty()) app.clipboard.copy_text(sel);
    }
  }
  end_sidebar_resize(app);
  if (app.sidebar_fav_dragging) {
    if (app.sidebar_fav_drag_from >= 0 && app.sidebar_fav_drag_to >= 0 &&
        app.sidebar_fav_drag_to != app.sidebar_fav_drag_from &&
        app.sidebar_fav_drag_to < static_cast<int>(app.favorites.size())) {
      std::string path = app.favorites[app.sidebar_fav_drag_from];
      app.favorites.erase(app.favorites.begin() + app.sidebar_fav_drag_from);
      app.favorites.insert(app.favorites.begin() + app.sidebar_fav_drag_to, path);
      save_file_browser_settings(app);
      refresh_sidebar(app);
    }
    app.sidebar_fav_dragging = false;
    app.sidebar_fav_drag_from = -1;
    app.sidebar_fav_drag_to = -1;
    app.sidebar_fav_drag_to_visual = -1;
    draw(app);
    return;
  }
  if (app.tab_dragging) {
    if (app.tab_drag_from >= 0 && app.tab_drag_to >= 0 &&
        app.tab_drag_to != app.tab_drag_from &&
        app.tab_drag_to < static_cast<int>(app.tabs.size())) {
      // Reorder tabs
      int from = app.tab_drag_from;
      int to = app.tab_drag_to;
      // Adjust active tab index
      if (app.active_tab == from) {
        app.active_tab = to;
      } else if (from < app.active_tab && to >= app.active_tab) {
        app.active_tab--;
      } else if (from > app.active_tab && to <= app.active_tab) {
        app.active_tab++;
      }
      Tab tab = std::move(app.tabs[from]);
      app.tabs.erase(app.tabs.begin() + from);
      app.tabs.insert(app.tabs.begin() + to, std::move(tab));
    }
    app.tab_dragging = false;
    app.tab_drag_from = -1;
    app.tab_drag_to = -1;
    app.tab_drag_to_visual = -1;
    draw(app);
    return;
  }
  if (app.tab_drag_from >= 0) {
    // Drag didn't start yet — cancel potential
    app.tab_drag_from = -1;
  }
  if (app.sidebar_fav_drag_from >= 0) {
    // Drag didn't start yet — cancel potential
    app.sidebar_fav_drag_from = -1;
    app.sidebar_fav_drag_to = -1;
    app.sidebar_fav_drag_to_visual = -1;
  }
  if (app.drag_potential) {
    cancel_drag(app);
  }
}


} // namespace eh::file_browser
