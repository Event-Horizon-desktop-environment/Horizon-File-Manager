// key_shortcuts.cpp — global key handlers (cancel op, tab shortcuts,
// type-to-find, space-preview dismiss, drop-chooser dismiss, Escape clear).
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

bool key_cancel_op(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
  if (sym == XKB_KEY_Escape && app.op_progress && app.op_progress->active) {
    app.op_progress->cancel = true;
    app.operation_status = "Cancelling...";
    app.operation_status_expires_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() + 3000;
    draw(app);
    return true;
  }
  return false;
}


bool key_global_shortcuts(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
  if (ctrl && (sym == XKB_KEY_T || sym == XKB_KEY_t)) {
    new_tab(app);
    draw(app);
    return true;
  }
  if (ctrl && (sym == XKB_KEY_W || sym == XKB_KEY_w)) {
    close_tab(app);
    draw(app);
    return true;
  }
  if (ctrl && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
    if (shift)
      prev_tab(app);
    else
      next_tab(app);
    draw(app);
    return true;
  }

  if (ctrl && shift && (sym == XKB_KEY_N || sym == XKB_KEY_n)) {
    app.create_dialog_open = true;
    app.create_is_folder = true;
    app.create_buf = "New Folder";
    app.create_cursor_pos = static_cast<int>(app.create_buf.size());
    app.create_hover_btn = -1;
    draw(app);
    return true;
  }

  if (ctrl && shift && (sym == XKB_KEY_T || sym == XKB_KEY_t)) {
    reopen_last_closed_tab(app);
    draw(app);
    return true;
  }

  if (ctrl && shift && (sym == XKB_KEY_I || sym == XKB_KEY_i)) {
    invert_selection(app);
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_C || sym == XKB_KEY_c)) {
    if (!app.cur_tab().multi_selected.empty()) {
      std::vector<std::string> paths;
      for (int vis_idx : app.cur_tab().multi_selected) {
        if (vis_idx >= 0 && vis_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
          int real_idx = app.cur_tab().visible_entries[vis_idx];
          if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()))
            paths.push_back(app.cur_tab().entries[real_idx].path);
        }
      }
      if (!paths.empty()) app.clipboard.copy_files(false, paths);
    } else if (app.cur_tab().selected_idx >= 0 &&
               app.cur_tab().selected_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real_idx = app.cur_tab().visible_entries[app.cur_tab().selected_idx];
      if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()))
        app.clipboard.copy_files(false, {app.cur_tab().entries[real_idx].path});
    }
    return true;
  }

  if (ctrl && (sym == XKB_KEY_X || sym == XKB_KEY_x)) {
    app.cut_paths.clear();
    if (!app.cur_tab().multi_selected.empty()) {
      std::vector<std::string> paths;
      for (int vis_idx : app.cur_tab().multi_selected) {
        if (vis_idx >= 0 && vis_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
          int real_idx = app.cur_tab().visible_entries[vis_idx];
          if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size())) {
            paths.push_back(app.cur_tab().entries[real_idx].path);
            app.cut_paths.insert(app.cur_tab().entries[real_idx].path);
          }
        }
      }
      if (!paths.empty()) app.clipboard.copy_files(true, paths);
    } else if (app.cur_tab().selected_idx >= 0 &&
               app.cur_tab().selected_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real_idx = app.cur_tab().visible_entries[app.cur_tab().selected_idx];
      if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size())) {
        app.clipboard.copy_files(true, {app.cur_tab().entries[real_idx].path});
        app.cut_paths.insert(app.cur_tab().entries[real_idx].path);
      }
    }
    app.pendingRedraw = true;
    return true;
  }

  if (ctrl && !shift && (sym == XKB_KEY_Z || sym == XKB_KEY_z)) {
    if (app.undo_stack.empty()) return true;
    auto rec = std::move(app.undo_stack.back());
    app.undo_stack.pop_back();
    std::error_code ec;
    switch (rec.type) {
      case AppState::UndoRecord::Type::PasteCopy:
        for (const auto& path : rec.paths_b) {
          if (fs::is_directory(path, ec)) fs::remove_all(path, ec);
          else fs::remove(path, ec);
        }
        break;
      case AppState::UndoRecord::Type::PasteCut:
        for (std::size_t i = 0; i < rec.paths_b.size() && i < rec.paths_a.size(); ++i) {
          fs::rename(rec.paths_b[i], rec.paths_a[i], ec);
        }
        break;
      case AppState::UndoRecord::Type::Rename:
        for (std::size_t i = 0; i < rec.paths_b.size() && i < rec.paths_a.size(); ++i) {
          fs::rename(rec.paths_b[i], rec.paths_a[i], ec);
        }
        break;
      case AppState::UndoRecord::Type::NewFolder:
        for (const auto& path : rec.paths_b) {
          fs::remove_all(path, ec);
        }
        break;
      case AppState::UndoRecord::Type::NewFile:
        for (const auto& path : rec.paths_b) {
          fs::remove(path, ec);
        }
        break;
      default:
        break;
    }
    app.redo_stack.push_back(std::move(rec));
    if (app.redo_stack.size() > app.kMaxUndo)
      app.redo_stack.erase(app.redo_stack.begin());
    reload_dir(app);
    draw(app);
    return true;
  }

  if (ctrl && ((sym == XKB_KEY_Y || sym == XKB_KEY_y) || (shift && (sym == XKB_KEY_Z || sym == XKB_KEY_z)))) {
    if (app.redo_stack.empty()) return true;
    auto rec = std::move(app.redo_stack.back());
    app.redo_stack.pop_back();
    std::error_code ec;
    switch (rec.type) {
      case AppState::UndoRecord::Type::PasteCopy:
        for (std::size_t i = 0; i < rec.paths_a.size() && i < rec.paths_b.size(); ++i) {
          if (!fs::exists(rec.paths_a[i], ec)) continue;
          fs::path dest = rec.paths_b[i];
          if (fs::is_directory(rec.paths_a[i], ec))
            fs::copy(rec.paths_a[i], dest, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
          else
            fs::copy_file(rec.paths_a[i], dest, fs::copy_options::copy_symlinks, ec);
        }
        break;
      case AppState::UndoRecord::Type::PasteCut:
        for (std::size_t i = 0; i < rec.paths_a.size() && i < rec.paths_b.size(); ++i) {
          fs::rename(rec.paths_a[i], rec.paths_b[i], ec);
        }
        break;
      case AppState::UndoRecord::Type::Rename:
        for (std::size_t i = 0; i < rec.paths_a.size() && i < rec.paths_b.size(); ++i) {
          fs::rename(rec.paths_a[i], rec.paths_b[i], ec);
        }
        break;
      case AppState::UndoRecord::Type::NewFolder:
        for (const auto& path : rec.paths_b) {
          fs::create_directory(path, ec);
        }
        break;
      case AppState::UndoRecord::Type::NewFile:
        for (const auto& path : rec.paths_b) {
          std::ofstream ofs(path);
          ofs.close();
        }
        break;
      default:
        break;
    }
    app.undo_stack.push_back(std::move(rec));
    if (app.undo_stack.size() > app.kMaxUndo)
      app.undo_stack.erase(app.undo_stack.begin());
    reload_dir(app);
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_V || sym == XKB_KEY_v)) {
    paste_clipboard(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_A || sym == XKB_KEY_a)) {
    app.cur_tab().multi_selected.clear();
    for (int i = 0; i < static_cast<int>(app.cur_tab().visible_entries.size()); ++i) {
      app.cur_tab().multi_selected.push_back(i);
    }
    if (!app.cur_tab().visible_entries.empty()) app.cur_tab().selected_idx = 0;
    app.cur_tab().selected_by_kbd = true;
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_H || sym == XKB_KEY_h)) {
    app.show_hidden = !app.show_hidden;
    save_file_browser_settings(app);
    reload_dir(app);
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_L || sym == XKB_KEY_l)) {
    auto& pe_editing = app.active_pane ? app.r_path_editing : app.path_editing;
    auto& pe_buf = app.active_pane ? app.r_path_edit_buf : app.path_edit_buf;
    auto& pe_cursor = app.active_pane ? app.r_path_edit_cursor : app.path_edit_cursor;
    auto& pe_sel_start = app.active_pane ? app.r_path_edit_sel_start : app.path_edit_sel_start;
    auto& pe_sel_end = app.active_pane ? app.r_path_edit_sel_end : app.path_edit_sel_end;
    pe_editing = true;
    pe_buf = app.cur_tab().current_path;
    pe_cursor = static_cast<int>(pe_buf.size());
    pe_sel_start = -1;
    pe_sel_end = -1;
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_F || sym == XKB_KEY_f)) {
    if ((app.active_pane ? app.r_search_active : app.search_active) || (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active)) {
      reset_search_filters(app);
      (app.active_pane ? app.r_search_active : app.search_active) = false;
      (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active) = false;
      (app.active_pane ? app.r_search_query : app.search_query).clear();
      (app.active_pane ? app.r_recursive_search_query : app.recursive_search_query).clear();
      recursive_search_worker().cancel();
      reload_dir(app);
    } else {
      (app.active_pane ? app.r_search_active : app.search_active) = true;
      (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active) = false;
      (app.active_pane ? app.r_search_query : app.search_query).clear();
      (app.active_pane ? app.r_recursive_search_query : app.recursive_search_query).clear();
      recursive_search_worker().cancel();
      (app.active_pane ? app.r_search_cursor : app.search_cursor) = 0;
      (app.active_pane ? app.r_search_sel_start : app.search_sel_start) = -1;
      (app.active_pane ? app.r_search_sel_end : app.search_sel_end) = -1;
    }
    (app.active_pane ? app.r_path_editing : app.path_editing) = false;
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_1)) {
    app.cur_tab().view_mode = ViewMode::List;
    app.last_browser_view_mode = ViewMode::List;
    save_file_browser_settings(app);
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_2)) {
    app.cur_tab().view_mode = ViewMode::Grid;
    app.last_browser_view_mode = ViewMode::Grid;
    save_file_browser_settings(app);
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_3)) {
    app.cur_tab().current_path = "computer://";
    navigate_to(app, "computer://");
    return true;
  }

  if (ctrl && (sym == XKB_KEY_4)) {
    app.cur_tab().view_mode = ViewMode::Tree;
    app.last_browser_view_mode = ViewMode::Tree;
    save_file_browser_settings(app);
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_5)) {
    app.cur_tab().view_mode = ViewMode::Compact;
    app.last_browser_view_mode = ViewMode::Compact;
    save_file_browser_settings(app);
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_G || sym == XKB_KEY_g)) {
    app.cur_tab().group_field =
        (app.cur_tab().group_field == 1) ? 0 : 1;
    app.cur_tab().group_by_type = (app.cur_tab().group_field == 1);
    reload_dir(app);
    save_file_browser_settings(app);
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_equal || sym == XKB_KEY_KP_Add)) {
    step_zoom(app, +1);
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_minus || sym == XKB_KEY_KP_Subtract)) {
    step_zoom(app, -1);
    draw(app);
    return true;
  }

  if (ctrl && (sym == XKB_KEY_0 || sym == XKB_KEY_KP_0)) {
    apply_zoom_pct(app, 100.0);
    draw(app);
    return true;
  }

  if (alt && (sym == XKB_KEY_Up || sym == XKB_KEY_KP_Up)) {
    navigate_up(app);
    return true;
  }

  if (alt && (sym == XKB_KEY_Left || sym == XKB_KEY_KP_Left)) {
    if (!app.cur_tab().nav_history.empty()) {
      navigate_back(app);
      return true;
    }
  }

  if (alt && (sym == XKB_KEY_Right || sym == XKB_KEY_KP_Right)) {
    if (!app.cur_tab().nav_forward.empty()) {
      navigate_forward(app);
      return true;
    }
  }

  if (sym == XKB_KEY_F3) {
    if (!app.split_view) {
      // With exactly one folder selected, split opens at that folder
      std::string sel_dir;
      const auto& tab = app.cur_tab();
      if (tab.selected_idx >= 0 &&
          tab.selected_idx < static_cast<int>(tab.visible_entries.size()) &&
          tab.multi_selected.size() <= 1) {
        int ri = tab.visible_entries[tab.selected_idx];
        if (ri >= 0 && ri < static_cast<int>(tab.entries.size()) &&
            tab.entries[ri].is_dir)
          sel_dir = tab.entries[ri].path;
      }
      enter_split_view(app, app.active_tab, sel_dir);
    } else {
      exit_split_view(app);
    }
    draw(app);
    return true;
  }

  if (sym == XKB_KEY_F5) {
    reload_dir(app);
    draw(app);
    return true;
  }

  if (sym == XKB_KEY_F11) {
    app.info_panel_open = !app.info_panel_open;
    if (app.info_panel_open) app.info_panel_needs_update = true;
    draw(app);
    return true;
  }
  return false;
}


bool key_type_to_find(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
  if (!(app.active_pane ? app.r_search_active : app.search_active) && !(app.active_pane ? app.r_path_editing : app.path_editing) && !app.settings_open &&
      !app.confirm_open && !app.create_dialog_open && !app.rename_ui_open &&
      !app.password_dialog_open &&
      !app.properties.open && !app.open_with_open && !app.compress_dialog_open &&
      !app.batch_rename_open && !app.settings_dropdown_open &&
      (app.active_pane ? !app.r_sort_menu_open : !app.sort_menu_open) && !app.context_menu_open && !app.drop_chooser_open &&
      !ctrl && !alt && utf8_len == 1 && utf8[0] >= 32 && utf8[0] < 127 &&
      app.cur_tab().current_path != "computer://") {
    auto& active = app.active_pane ? app.r_search_active : app.search_active;
    auto& recursive = app.active_pane ? app.r_recursive_search_active : app.recursive_search_active;
    auto& query = app.active_pane ? app.r_search_query : app.search_query;
    auto& rec_query = app.active_pane ? app.r_recursive_search_query : app.recursive_search_query;
    auto& cursor = app.active_pane ? app.r_search_cursor : app.search_cursor;
    auto& sel_start = app.active_pane ? app.r_search_sel_start : app.search_sel_start;
    auto& sel_end = app.active_pane ? app.r_search_sel_end : app.search_sel_end;
    active = true;
    recursive = false;
    query.clear();
    rec_query.clear();
    cursor = 0;
    sel_start = -1;
    sel_end = -1;
    query += utf8[0];
    cursor = 1;
    app.cur_tab().entries.clear();
    app.cur_tab().visible_entries.clear();
    app.cur_tab().tree_entries_dirty = true;
    recursive_search_worker().start_search(app.cur_tab().current_path, query);
    draw(app);
    return true;
  }
  return false;
}


bool key_space_dismiss(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
  if (app.preview_mode == AppState::PreviewMode::Space && sym == XKB_KEY_Escape) {
    reset_preview(app);
    draw(app);
    return true;
  }
  return false;
}


bool key_drop_dismiss(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
  if (app.drop_chooser_open && sym == XKB_KEY_Escape) {
    app.drop_chooser_open = false;
    app.drop_chooser_hover = -1;
    app.drop_chooser_srcs.clear();
    app.drop_chooser_target.clear();
    draw(app);
    return true;
  }
  return false;
}


bool key_escape_clear(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
  if (sym == XKB_KEY_Escape) {
    if (!app.cut_paths.empty()) {
      app.cut_paths.clear();
      draw(app);
      return true;
    }
  }
  return false;
}


} // namespace eh::file_browser
