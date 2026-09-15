// key_nav.cpp — main file-browser navigation keys (the big switch).
// Moved verbatim out of keyboard.cpp handle_key (each region keeps its
// exact source body; the dispatcher calls these in flow order).
#include "events.hpp"
#include "../app.hpp"
#include "../ui/layout.hpp"
#include "../features/compress.hpp"
#include "../features/drag.hpp"
#include "../features/progress.hpp"
#include "../features/query_match.hpp"
#include "../features/recursive_search_worker.hpp"
#include "../features/selection.hpp"
#include "../features/sidebar.hpp"
#include "../features/tab_history.hpp"
#include "../features/tags.hpp"
#include "../features/view_zoom.hpp"

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
// ── key region handlers, in original flow order ─────────────────────────────

// ── grouped-header row helpers (moved from keyboard.cpp; consumed only by
// key_navigate) ───────────────────────────────────────────────────────────
static int headers_before(AppState const& app, int vi) {
  if (!app.cur_tab().group_by_type) return 0;
  int count = 0, prev = -1;
  for (int i = 0; i <= vi; ++i) {
    int r = app.cur_tab().visible_entries[i];
    if (r < 0 || r >= static_cast<int>(app.cur_tab().entries.size())) continue;
    int t = static_cast<int>(app.cur_tab().entries[r].type);
    if (t != prev) { ++count; prev = t; }
  }
  return count;
}

static int header_h(AppState const& app) {
  return static_cast<int>(app.entry_height * 0.55);
}

static int entry_top(AppState const& app, int vi) {
  return vi * app.entry_height + headers_before(app, vi) * header_h(app);
}

static int entry_bottom(AppState const& app, int vi) {
  return (vi + 1) * app.entry_height + headers_before(app, vi) * header_h(app);
}


bool key_navigate(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
  switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      open_selected(app);
      draw(app);
      return true;

    case XKB_KEY_BackSpace:
      navigate_up(app);
      return true;

    case XKB_KEY_space:
      toggle_space_preview(app);
      draw(app);
      return true;

    case XKB_KEY_Left: {
      app.cur_tab().selected_by_kbd = true;
      // Tree view: collapse expanded directory
      if (app.cur_tab().view_mode == ViewMode::Tree) {
        auto& tab = app.cur_tab();
        if (tab.selected_idx >= 0 && tab.selected_idx < static_cast<int>(tab.tree_entries.size())) {
          auto& te = tab.tree_entries[tab.selected_idx];
          if (te.is_dir && tab.tree_expanded.count(te.path)) {
            tab.tree_expanded.erase(te.path);
            build_tree_entries(app);
          } else {
            // Navigate to parent: find an entry with lower depth
            for (int i = tab.selected_idx - 1; i >= 0; --i) {
              if (tab.tree_entries[i].depth < te.depth) {
                tab.selected_idx = i;
                tab.multi_selected = {i};
                break;
              }
            }
          }
        }
        draw(app);
        return true;
      }
      if (app.preview_mode == AppState::PreviewMode::Space) {
        int idx = std::max(0, app.cur_tab().selected_idx - 1);
        app.cur_tab().selected_idx = idx;
        app.cur_tab().multi_selected = {idx};
        activate_space_preview(app);
        draw(app);
        return true;
      }
      int idx = app.cur_tab().selected_idx >= 0 ? app.cur_tab().selected_idx : 0;
      idx = std::max(0, idx - 1);
      app.cur_tab().selected_idx = idx;
      app.cur_tab().multi_selected = {idx};
      if (app.cur_tab().view_mode == ViewMode::Grid) {
        GridLayout gl = compute_grid_layout(
            pane_view_rect_for_index(app, app.active_pane).w,
            app.zoom_pct / 100.0);
        int row_h = gl.row_h;
        int cols = std::max(1, gl.cols);
        int top_gap = gl.row_gap;
        int target_line = top_gap + (idx / cols) * row_h;
        if (target_line < app.cur_tab().scroll_px)
          app.cur_tab().scroll_px = target_line;
      } else {
        int target_line = entry_top(app, idx);
        if (target_line < app.cur_tab().scroll_px)
          app.cur_tab().scroll_px = target_line;
      }
      draw(app);
      return true;
    }

    case XKB_KEY_Right: {
      app.cur_tab().selected_by_kbd = true;
      // Tree view: expand collapsed directory
      if (app.cur_tab().view_mode == ViewMode::Tree) {
        auto& tab = app.cur_tab();
        if (tab.selected_idx >= 0 && tab.selected_idx < static_cast<int>(tab.tree_entries.size())) {
          auto& te = tab.tree_entries[tab.selected_idx];
          if (te.is_dir && !tab.tree_expanded.count(te.path)) {
            tab.tree_expanded.insert(te.path);
            build_tree_entries(app);
            draw(app);
          } else {
            open_selected(app);
          }
        }
        draw(app);
        return true;
      }
      if (app.preview_mode == AppState::PreviewMode::Space) {
        int idx = app.cur_tab().selected_idx + 1;
        int max_i = static_cast<int>(app.cur_tab().visible_entries.size()) - 1;
        if (idx > max_i) idx = max_i;
        app.cur_tab().selected_idx = idx;
        app.cur_tab().multi_selected = {idx};
        activate_space_preview(app);
        draw(app);
        return true;
      }
      int idx = app.cur_tab().selected_idx >= 0 ? app.cur_tab().selected_idx : -1;
      idx = std::min(static_cast<int>(app.cur_tab().visible_entries.size()) - 1, idx + 1);
      app.cur_tab().selected_idx = idx;
      app.cur_tab().multi_selected = {idx};
      if (app.cur_tab().view_mode == ViewMode::Grid) {
        GridLayout gl = compute_grid_layout(
            pane_view_rect_for_index(app, app.active_pane).w,
            app.zoom_pct / 100.0);
        int row_h = gl.row_h;
        int cols = std::max(1, gl.cols);
        int top_gap = gl.row_gap;
        int view_h = app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height;
        int target_line = top_gap + (idx / cols + 1) * row_h;
        if (target_line > app.cur_tab().scroll_px + view_h)
          app.cur_tab().scroll_px = target_line - view_h;
      } else {
        int view_h = app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height;
        int target_line = entry_bottom(app, idx);
        if (target_line > app.cur_tab().scroll_px + view_h)
          app.cur_tab().scroll_px = target_line - view_h;
      }
      draw(app);
      return true;
    }

    case XKB_KEY_Up: {
      app.cur_tab().selected_by_kbd = true;
      // Tree view: navigate tree_entries
      if (app.cur_tab().view_mode == ViewMode::Tree) {
        auto& tab = app.cur_tab();
        int idx = tab.selected_idx >= 0 ? tab.selected_idx : 0;
        idx = std::max(0, idx - 1);
        tab.selected_idx = idx;
        tab.multi_selected = {idx};
        int view_h = app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height;
        double zf = app.zoom_pct / 100.0;
        int entry_h = static_cast<int>(28.0 * zf);
        int target_line = idx * entry_h;
        if (target_line < tab.scroll_px)
          tab.scroll_px = target_line;
        draw(app);
        return true;
      }
      int idx = app.cur_tab().selected_idx >= 0 ? app.cur_tab().selected_idx : 0;
      if (app.cur_tab().view_mode == ViewMode::Grid) {
        GridLayout gl = compute_grid_layout(
            pane_view_rect_for_index(app, app.active_pane).w,
            app.zoom_pct / 100.0);
        int row_h = gl.row_h;
        int cols = std::max(1, gl.cols);
        int top_gap = gl.row_gap;
        idx = std::max(0, idx - cols);
        app.cur_tab().selected_idx = idx;
        app.cur_tab().multi_selected = {idx};
        int target_line = top_gap + (idx / cols) * row_h;
        if (target_line < app.cur_tab().scroll_px)
          app.cur_tab().scroll_px = target_line;
      } else {
        idx = std::max(0, idx - 1);
        app.cur_tab().selected_idx = idx;
        app.cur_tab().multi_selected = {idx};
        int target_line = entry_top(app, idx);
        if (target_line < app.cur_tab().scroll_px)
          app.cur_tab().scroll_px = target_line;
      }
      draw(app);
      return true;
    }

    case XKB_KEY_Down: {
      app.cur_tab().selected_by_kbd = true;
      // Tree view: navigate tree_entries
      if (app.cur_tab().view_mode == ViewMode::Tree) {
        auto& tab = app.cur_tab();
        int idx = tab.selected_idx >= 0 ? tab.selected_idx : -1;
        int max_idx = static_cast<int>(tab.tree_entries.size()) - 1;
        idx = std::min(max_idx, idx + 1);
        tab.selected_idx = idx;
        tab.multi_selected = {idx};
        double zf = app.zoom_pct / 100.0;
        int entry_h = static_cast<int>(28.0 * zf);
        int target_line = (idx + 1) * entry_h;
        int view_h = app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height;
        if (target_line > tab.scroll_px + view_h)
          tab.scroll_px = target_line - view_h;
        draw(app);
        return true;
      }
      int idx = app.cur_tab().selected_idx >= 0 ? app.cur_tab().selected_idx : -1;
      int max_idx = static_cast<int>(app.cur_tab().visible_entries.size()) - 1;
      if (app.cur_tab().view_mode == ViewMode::Grid) {
        GridLayout gl = compute_grid_layout(
            pane_view_rect_for_index(app, app.active_pane).w,
            app.zoom_pct / 100.0);
        int row_h = gl.row_h;
        int cols = std::max(1, gl.cols);
        int top_gap = gl.row_gap;
        idx = std::min(max_idx, idx + cols);
        app.cur_tab().selected_idx = idx;
        app.cur_tab().multi_selected = {idx};
        int view_h = app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height;
        int target_line = top_gap + (idx / cols + 1) * row_h;
        if (target_line > app.cur_tab().scroll_px + view_h)
          app.cur_tab().scroll_px = target_line - view_h;
      } else {
        idx = std::min(max_idx, idx + 1);
        app.cur_tab().selected_idx = idx;
        app.cur_tab().multi_selected = {idx};
        int target_line = entry_bottom(app, idx);
        int view_h = app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height;
        if (target_line > app.cur_tab().scroll_px + view_h)
          app.cur_tab().scroll_px = target_line - view_h;
      }
      draw(app);
      return true;
    }

    case XKB_KEY_F2: {
      auto targets = app.cur_tab().multi_selected.empty()
        ? std::vector<int>{}
        : app.cur_tab().multi_selected;
      if (!targets.empty()) {
        // Batch rename
        app.batch_rename_entries.clear();
        for (int vis_idx : targets) {
          if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size())) continue;
          int real_idx = app.cur_tab().visible_entries[vis_idx];
          if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size())) continue;
          const auto& e = app.cur_tab().entries[real_idx];
          std::string ext;
          std::string base;
          auto dot = e.name.rfind('.');
          if (dot == std::string::npos || dot == 0) {
            ext.clear();
            base = e.name;
          } else {
            ext = e.name.substr(dot);
            base = e.name.substr(0, dot);
          }
          app.batch_rename_entries.push_back({e.path, e.name, ext, e.name});
        }
        if (app.batch_rename_entries.empty()) return true;
        app.batch_rename_mode = 0;
        app.batch_rename_template = "[Original filename]";
        app.batch_rename_template_cursor = static_cast<int>(app.batch_rename_template.size());
        app.batch_rename_find.clear();
        app.batch_rename_find_cursor = 0;
        app.batch_rename_replace.clear();
        app.batch_rename_replace_cursor = 0;
        app.batch_rename_show_add = false;
        app.batch_rename_add_hover = -1;
        app.batch_rename_hover_btn = -1;
        app.batch_rename_hover_mode = -1;
        app.batch_rename_edit_focus = 0;
        app.batch_rename_open = true;
        draw(app);
        return true;
      }
      // Single rename (existing behavior)
      if (app.cur_tab().selected_idx < 0 ||
          app.cur_tab().selected_idx >= static_cast<int>(app.cur_tab().visible_entries.size()))
        return true;
      int real_idx = app.cur_tab().visible_entries[app.cur_tab().selected_idx];
      if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
        return true;
      auto& entry = app.cur_tab().entries[real_idx];
      app.rename_ui_open = true;
      app.rename_ui_old_name = entry.name;
      app.rename_ui_buf = entry.name;
      app.rename_ui_cursor_pos = static_cast<int>(entry.name.size());
      app.rename_ui_entry_path = entry.path;
      app.rename_ui_hover_btn = -1;
      draw(app);
      return true;
    }

    case XKB_KEY_Delete: {
      auto targets = app.cur_tab().multi_selected.empty()
        ? std::vector<int>{app.cur_tab().selected_idx}
        : app.cur_tab().multi_selected;
      std::vector<std::string> del_paths;
      for (int vis_idx : targets) {
        if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size()))
          continue;
        int real_idx = app.cur_tab().visible_entries[vis_idx];
        if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
          continue;
        del_paths.push_back(app.cur_tab().entries[real_idx].path);
      }
      if (del_paths.empty()) return true;
      if (shift) {
        app.confirm_title = "Permanently Delete?";
        app.confirm_message = (del_paths.size() == 1)
          ? "Permanently delete \"" + fs::path(del_paths[0]).filename().string() + "\"?"
          : "Permanently delete " + std::to_string(del_paths.size()) + " items?";
        app.confirm_preview_path = del_paths.size() == 1 ? del_paths[0] : "";
        app.confirm_item_count = static_cast<int>(del_paths.size());
        app.confirm_callback = [&app, paths = std::move(del_paths)](bool ok) {
          if (!ok) return;
          std::error_code ec;
          for (const auto& p : paths) {
            bool is_dir = fs::is_directory(p, ec);
            if (is_dir) fs::remove_all(p, ec); else fs::remove(p, ec);
          }
          reload_dir(app);
        };
      } else {
        app.confirm_title = "Move to Trash";
        app.confirm_message = (del_paths.size() == 1)
          ? "Move \"" + fs::path(del_paths[0]).filename().string() + "\" to trash?"
          : "Move " + std::to_string(del_paths.size()) + " items to trash?";
        app.confirm_preview_path = del_paths.size() == 1 ? del_paths[0] : "";
        app.confirm_item_count = static_cast<int>(del_paths.size());
        app.confirm_callback = [&app, paths = std::move(del_paths)](bool ok) {
          if (!ok) return;
          for (const auto& p : paths) (void)xdg::trash_file(p);
          reload_dir(app);
        };
      }
      app.confirm_hover_btn = -1;
      app.confirm_open = true;
      draw(app);
      return true;
    }
  }
  return false;
}


} // namespace eh::file_browser
