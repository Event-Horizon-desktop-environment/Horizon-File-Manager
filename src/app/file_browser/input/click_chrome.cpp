// click_chrome.cpp — chrome split from click.cpp (handle_click region carve).
// Block bodies byte-identical to click.cpp; bare `return;` converted
// to `return true;`; trailing `return false;` marks fall-through.
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

namespace fs = std::filesystem;

namespace eh::file_browser {

// True when a modal surface covers the content area and should swallow the
// click instead of the scrollbar beneath it.
static bool modal_blocks_content(const AppState& app) {
  return app.create_dialog_open || app.select_pattern_open ||
         app.rename_ui_open || app.batch_rename_open || app.confirm_open ||
         app.conflict_open || app.password_dialog_open ||
         app.compress_dialog_open || app.term_chooser_open ||
         app.open_with_open || app.context_menu_open || app.settings_open ||
         app.properties.open || app.sort_menu_open || app.r_sort_menu_open ||
         app.columns_menu_open || app.r_columns_menu_open ||
         app.filter_dropdown_section > 0 || app.r_filter_dropdown_section > 0 ||
         app.ops_panel_slide > 0.01;
}


bool click_scrollbar(AppState& app, int x, int y, int button) {
  // ── Main-view scrollbar: grab thumb / jump-to-tap ──
  if (button == 0x110 && !modal_blocks_content(app)) {
    for (const auto& sb : app.scrollbar_rects) {
      // Forgiving strip: a few px on each side of the 6px track.
      if (x < sb.x - 6 || x >= sb.x + sb.w + 8) continue;
      if (y < sb.y || y >= sb.y + sb.h) continue;

      double track_h = static_cast<double>(sb.h);
      double thumb_h = std::max(static_cast<double>(sb.view_h) * sb.view_h /
                                    static_cast<double>(sb.content_h),
                                20.0);
      int max_scroll = std::max(0, sb.content_h - sb.view_h);
      int cur = sb.computer ? app.computer_scroll_px : app.cur_tab().scroll_px;
      double thumb_y =
          max_scroll > 0
              ? sb.y + static_cast<double>(cur) / max_scroll * (track_h - thumb_h)
              : sb.y;

      app.scrollbar_dragging = true;
      app.scrollbar_drag_rect = sb;
      app.scrollbar_grab_dy = (y >= thumb_y && y <= thumb_y + thumb_h)
                                  ? static_cast<int>(y - thumb_y)
                                  : static_cast<int>(thumb_h / 2.0);
      apply_scrollbar_drag(app, y);
      draw(app);
      return true;
    }
  }
  return false;
}

bool click_path_edit_cancel(AppState& app, int x, int y, int button) {
  // ── Click outside the nav/path field cancels path editing ──
  if ((app.active_pane ? app.r_path_editing : app.path_editing) && button == 0x110) {
    double zf = app.zoom_pct / 100.0;
    int bar_y = y;
    if (app.split_view) {
      int content_y = app.top_bar_height + app.tab_bar_height;
      if (y >= content_y && y < content_y + app.top_bar_height)
        bar_y = y - content_y;
    }
    int arrow_w = static_cast<int>(36.0 * zf);
    int gap4 = static_cast<int>(6.0 * zf);
    int mx6 = static_cast<int>(24.0 * zf);
    int path_pad = static_cast<int>(12.0 * zf);
    int house_w = static_cast<int>(16.0 * zf);
    int gap12 = static_cast<int>(12.0 * zf);
    int nav_origin = app.nav_origin_x();
    int path_x = nav_origin + 3 * arrow_w + 2 * gap4 + mx6 + path_pad + house_w + gap12;
    auto& in_search_btn_x = app.active_pane ? app.r_search_btn_x : app.search_btn_x;
    int path_w = in_search_btn_x - static_cast<int>(6.0 * zf) - path_x;
    bool in_nav_field =
        bar_y >= 0 && bar_y < app.top_bar_height && x >= path_x && x < path_x + path_w;
    if (!in_nav_field) {
      (app.active_pane ? app.r_path_editing : app.path_editing) = false;
      (app.active_pane ? app.r_path_edit_dragging : app.path_edit_dragging) = false;
      (app.active_pane ? app.r_path_edit_sel_start : app.path_edit_sel_start) = -1;
      (app.active_pane ? app.r_path_edit_sel_end : app.path_edit_sel_end) = -1;
      draw(app);
    }
  }
  return false;
}

bool click_status_zoom(AppState& app, int x, int y, int button) {
  // ── Status-bar zoom controls (− / track / +) ──
  if ((app.status_zoom_slider_w > 0 || app.status_zoom_minus[2] > 0 ||
       app.status_zoom_plus[2] > 0) &&
      y >= app.height - app.status_bar_height) {
    if (button == 0x110) {
      auto in_rect = [&](const int* r) {
        return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
      };
      if (app.status_zoom_minus[2] > 0 && in_rect(app.status_zoom_minus)) {
        step_zoom(app, -1);
        save_file_browser_settings(app);
        draw(app);
        return true;
      }
      if (app.status_zoom_plus[2] > 0 && in_rect(app.status_zoom_plus)) {
        step_zoom(app, +1);
        save_file_browser_settings(app);
        draw(app);
        return true;
      }
      if (x >= app.status_zoom_slider_x &&
          x < app.status_zoom_slider_x + app.status_zoom_slider_w) {
        // Relative drag: anchor at press point, no jump-to-position.
        app.status_zoom_press_x = x;
        app.status_zoom_press_level =
            zoom_level_for_pct(app.settings_zoom_pct);
        app.status_zoom_last_level = app.status_zoom_press_level;
        app.status_zoom_dragging = true;
        draw(app);
      }
    }
    return true;
  }
  return false;
}

bool click_picker_bar(AppState& app, int x, int y, int button) {
  // ── Picker bar buttons (dir or file) ──
  if ((app.select_dir_mode || app.select_file_mode) && button == 0x110) {
    if (y >= app.select_bar_y && y < app.select_bar_y + app.select_bar_h) {
      if (x >= app.select_btn_x && x < app.select_btn_x + app.select_btn_w) {
        if (app.select_file_mode) {
          auto& tab = app.cur_tab();
          if (tab.selected_idx >= 0 && tab.selected_idx < static_cast<int>(tab.visible_entries.size())) {
            auto& fe = tab.entries[tab.visible_entries[tab.selected_idx]];
            app.select_dir_result = fe.path;
            app.running = false;
          }
        } else {
          app.select_dir_result = app.cur_tab().current_path;
          app.running = false;
        }
        return true;
      }
      if (x >= app.cancel_btn_x && x < app.cancel_btn_x + app.cancel_btn_w) {
        app.running = false;
        return true;
      }
    }
  }
  return false;
}

bool click_info_tab(AppState& app, int x, int y, int button) {
  // ── Info panel tab click (in the left‑click handler) ──
  if (app.info_panel_open) {
    int panel_px = app.width - app.info_panel_width;
    int top_h = app.top_bar_height + app.tab_bar_height;
    int tab_h = static_cast<int>(38 * app.zoom_pct / 100.0);
    if (x >= panel_px && y >= top_h && y < top_h + tab_h) {
      for (int i = 0; i < 3; ++i) {
        if (x >= static_cast<int>(app.info_panel_hit_tabs[i][0]) &&
            x < static_cast<int>(app.info_panel_hit_tabs[i][0] + app.info_panel_hit_tabs[i][2]) &&
            y >= static_cast<int>(app.info_panel_hit_tabs[i][1]) &&
            y < static_cast<int>(app.info_panel_hit_tabs[i][1] + app.info_panel_hit_tabs[i][3])) {
          app.info_panel_tab = i;
          draw(app);
          return true;
        }
      }
    }
  }
  return false;
}

bool click_sidebar_drag(AppState& app, int x, int y, int button) {
  // ── Sidebar drag start ──
  if (button == 0x110 && begin_sidebar_resize(app, x)) {
    return true;
  }
  return false;
}

bool click_columns_menu(AppState& app, int x, int y, int button) {
  // ── Sort menu item click (handle before top bar, so menu stays on top) ──
  // ── Column chooser popup clicks ──
  if ((app.active_pane ? app.r_columns_menu_open : app.columns_menu_open)) {
    auto& cmo = app.active_pane ? app.r_columns_menu_open : app.columns_menu_open;
    auto& cmx = app.active_pane ? app.r_columns_menu_x : app.columns_menu_x;
    auto& cmy = app.active_pane ? app.r_columns_menu_y : app.columns_menu_y;
    auto& cmw = app.active_pane ? app.r_columns_menu_w : app.columns_menu_w;
    auto& cmh = app.active_pane ? app.r_columns_menu_h : app.columns_menu_h;
    if (x >= cmx && x < cmx + cmw && y >= cmy && y < cmy + cmh) {
      int rel_y = y - cmy - kSortMenuPad;
      int idx = rel_y / kSortMenuItemH;
      bool changed = false;
      switch (idx) {
        case 0: app.col_owner = !app.col_owner; changed = true; break;
        case 1: app.col_group = !app.col_group; changed = true; break;
        case 2: app.col_perms = !app.col_perms; changed = true; break;
        case 3: app.col_ext = !app.col_ext; changed = true; break;
        case 4: app.col_target = !app.col_target; changed = true; break;
        default: break;
      }
      if (changed) {
        cmo = false;
        save_file_browser_settings(app);
        draw(app);
      }
      return true;
    } else {
      cmo = false;
      draw(app);
      return true;
    }
  }
  return false;
}

bool click_sort_menu(AppState& app, int x, int y, int button) {
  if ((app.active_pane ? app.r_sort_menu_open : app.sort_menu_open)) {
    if (x >= (app.active_pane ? app.r_sort_menu_x : app.sort_menu_x) && x < (app.active_pane ? app.r_sort_menu_x : app.sort_menu_x) + (app.active_pane ? app.r_sort_menu_w : app.sort_menu_w) &&
        y >= (app.active_pane ? app.r_sort_menu_y : app.sort_menu_y) && y < (app.active_pane ? app.r_sort_menu_y : app.sort_menu_y) + (app.active_pane ? app.r_sort_menu_h : app.sort_menu_h)) {
      int rel_y = y - (app.active_pane ? app.r_sort_menu_y : app.sort_menu_y) - kSortMenuPad;
      int idx = rel_y / kSortMenuItemH +
                (app.active_pane ? app.r_sort_menu_scroll : app.sort_menu_scroll);
      if (idx >= 0 && idx < sort_menu_row_count()) {
        const SortMenuRow& row = sort_menu_row(idx);
        bool changed = false;
        switch (row.kind) {
          case SortMenuRow::Kind::Field:
            app.cur_tab().sort_field = static_cast<SortField>(row.field);
            changed = true;
            break;
          case SortMenuRow::Kind::ToggleDescending:
            app.cur_tab().sort_descending = !app.cur_tab().sort_descending;
            changed = true;
            break;
          case SortMenuRow::Kind::ToggleFoldersFirst:
            app.folders_before_files = !app.folders_before_files;
            changed = true;
            break;
          case SortMenuRow::Kind::ToggleHiddenLast:
            app.sort_hidden_last = !app.sort_hidden_last;
            changed = true;
            break;
          case SortMenuRow::Kind::ToggleNatural:
            app.sort_natural = !app.sort_natural;
            changed = true;
            break;
          case SortMenuRow::Kind::ToggleCaseSensitive:
            app.sort_case_sensitive = !app.sort_case_sensitive;
            changed = true;
            break;
          case SortMenuRow::Kind::GroupField:
            app.cur_tab().group_field = row.field;
            app.cur_tab().group_by_type = (row.field == 1);
            changed = true;
            break;
          default:
            break; // separator: keep menu open
        }
        if (changed) {
          (app.active_pane ? app.r_sort_menu_open : app.sort_menu_open) = false;
          save_file_browser_settings(app);
          reload_dir(app);
          draw(app);
        }
        return true;
      }
    } else {
      (app.active_pane ? app.r_sort_menu_open : app.sort_menu_open) = false;
      draw(app);
      return true;
    }
  }
  return false;
}

bool click_filter_dropdown(AppState& app, int x, int y, int button) {
  // ── Filter dropdown click (handled before top bar) ──
  auto& click_filter_dd_x = app.active_pane ? app.r_filter_dropdown_x : app.filter_dropdown_x;
  auto& click_filter_dd_y = app.active_pane ? app.r_filter_dropdown_y : app.filter_dropdown_y;
  auto& click_filter_dd_w = app.active_pane ? app.r_filter_dropdown_w : app.filter_dropdown_w;
  auto& click_filter_dd_h = app.active_pane ? app.r_filter_dropdown_h : app.filter_dropdown_h;
  auto& click_filter_section = app.active_pane ? app.r_filter_dropdown_section : app.filter_dropdown_section;
  auto& click_filter_hover = app.active_pane ? app.r_filter_dropdown_hover : app.filter_dropdown_hover;
  auto& click_filter_type = app.active_pane ? app.r_filter_type_idx : app.filter_type_idx;
  auto& click_filter_size = app.active_pane ? app.r_filter_size_idx : app.filter_size_idx;
  auto& click_filter_date = app.active_pane ? app.r_filter_date_idx : app.filter_date_idx;
  if (click_filter_section > 0) {
    if (x >= click_filter_dd_x && x < click_filter_dd_x + click_filter_dd_w &&
        y >= click_filter_dd_y && y < click_filter_dd_y + click_filter_dd_h) {
      // Map click to global index, then to section+item or header
      int rel_y = y - click_filter_dd_y - kFilterPD;
      int gy = 0;
      int section = click_filter_section;
      int clicked_section = 0, clicked_item = -1;
      for (int si = 1; si <= 3; ++si) {
        // Header
        if (rel_y >= gy && rel_y < gy + kFilterHdrH) {
          clicked_section = si;
          clicked_item = -1; // header click
          break;
        }
        gy += kFilterHdrH;

        // Items if expanded
        if (section == si) {
          int cnt = (si == 1) ? 13 : (si == 2) ? 7 : 5;
          int item_y = gy;
          for (int i = 0; i < cnt; ++i) {
            if (rel_y >= item_y && rel_y < item_y + kFilterItemH) {
              clicked_section = si;
              clicked_item = i;
              break;
            }
            item_y += kFilterItemH;
          }
          gy = item_y;
          if (clicked_item >= 0) break;
        }

        gy += kFilterSep;
      }

      if (clicked_item >= 0) {
        // Item clicked — select it and close dropdown
        if (clicked_section == 1) click_filter_type = clicked_item;
        else if (clicked_section == 2) click_filter_size = clicked_item;
        else if (clicked_section == 3) click_filter_date = clicked_item;
        click_filter_section = 0;
        trigger_search_on_filter_change(app);
        draw(app);
        return true;
      } else if (clicked_section > 0) {
        // Header clicked — toggle expansion
        click_filter_section = (click_filter_section == clicked_section) ? 0 : clicked_section;
        click_filter_hover = -1;
        draw(app);
        return true;
      }
    } else {
      click_filter_section = 0;
      draw(app);
      return true;
    }
  }
  return false;
}

bool click_top_bar(AppState& app, int x, int y, int button) {
  int bar_y = y;
  if (app.split_view) {
    int content_y = app.top_bar_height + app.tab_bar_height;
    if (y >= content_y && y < content_y + app.top_bar_height)
      bar_y = y - content_y;
  }
  if (bar_y < app.top_bar_height) {
    app.last_click_ns = 0;
    double zf = app.zoom_pct / 100.0;

    auto& in_search_btn_x = app.active_pane ? app.r_search_btn_x : app.search_btn_x;
    auto& in_search_btn_w = app.active_pane ? app.r_search_btn_w : app.search_btn_w;
    auto& in_folder_search_btn_x = app.active_pane ? app.r_folder_search_btn_x : app.folder_search_btn_x;
    auto& in_folder_search_btn_w = app.active_pane ? app.r_folder_search_btn_w : app.folder_search_btn_w;
    auto& in_view_btn_x = app.active_pane ? app.r_view_btn_x : app.view_btn_x;
    auto& in_view_btn_w = app.active_pane ? app.r_view_btn_w : app.view_btn_w;
    auto& in_sort_btn_x = app.active_pane ? app.r_sort_btn_x : app.sort_btn_x;
    auto& in_sort_btn_w = app.active_pane ? app.r_sort_btn_w : app.sort_btn_w;
    auto& in_dots_btn_x = app.active_pane ? app.r_dots_btn_x : app.dots_btn_x;
    auto& in_dots_btn_y = app.active_pane ? app.r_dots_btn_y : app.dots_btn_y;
    auto& in_dots_btn_w = app.active_pane ? app.r_dots_btn_w : app.dots_btn_w;
    auto& in_dots_btn_h = app.active_pane ? app.r_dots_btn_h : app.dots_btn_h;
    auto& in_arrow_back_x = app.active_pane ? app.r_arrow_back_x : app.arrow_back_x;
    auto& in_arrow_forward_x = app.active_pane ? app.r_arrow_forward_x : app.arrow_forward_x;
    auto& in_search_bar_x = app.active_pane ? app.r_search_bar_x : app.search_bar_x;
    auto& in_search_bar_w = app.active_pane ? app.r_search_bar_w : app.search_bar_w;
    auto& in_search_clear_x = app.active_pane ? app.r_search_clear_x : app.search_clear_x;
    auto& in_search_clear_w = app.active_pane ? app.r_search_clear_w : app.search_clear_w;
    auto& in_filter_btn_x = app.active_pane ? app.r_filter_btn_x : app.filter_btn_x;
    auto& in_filter_btn_w = app.active_pane ? app.r_filter_btn_w : app.filter_btn_w;
    auto& in_breadcrumbs = app.active_pane ? app.r_breadcrumbs : app.breadcrumbs;
    auto& in_breadcrumb_hover = app.active_pane ? app.r_breadcrumb_hover : app.breadcrumb_hover;
    auto& in_search_active = app.active_pane ? app.r_search_active : app.search_active;
    auto& in_recursive_search_active = app.active_pane ? app.r_recursive_search_active : app.recursive_search_active;
    auto& in_search_query = app.active_pane ? app.r_search_query : app.search_query;
    auto& in_recursive_search_query = app.active_pane ? app.r_recursive_search_query : app.recursive_search_query;
    auto& in_search_cursor = app.active_pane ? app.r_search_cursor : app.search_cursor;
    auto& in_search_sel_start = app.active_pane ? app.r_search_sel_start : app.search_sel_start;
    auto& in_search_sel_end = app.active_pane ? app.r_search_sel_end : app.search_sel_end;

    // Window control buttons (traffic lights on the right: max | min | close)
    if (app.win_btn_x > 0) {
      int rel = x - app.win_btn_x;
      if (rel >= 0 && rel < 3 * app.win_btn_w) {
        int slot = rel / app.win_btn_w;
        if (slot == 0) {
          // Maximize (green)
          if (app.toplevel) {
            xdg_toplevel_set_maximized(app.toplevel);
            wl_display_flush(app.wl.display());
          }
        } else if (slot == 1) {
          // Minimize (yellow)
          if (app.toplevel) {
            xdg_toplevel_set_minimized(app.toplevel);
            wl_display_flush(app.wl.display());
          }
        } else {
          // Close (red)
          app.running = false;
        }
        return true;
      }
    }

    // Settings gear button
    {
      int gap4 = static_cast<int>(6.0 * zf);
      int gear_w = static_cast<int>(36.0 * zf);
      int gear_x = in_sort_btn_x + in_sort_btn_w + gap4;
      if (x >= gear_x && x < gear_x + gear_w) {
        open_settings(app);
        draw(app);
        return true;
      }
    }

    // Path bar dots menu button
    if (in_dots_btn_w > 0 &&
        x >= in_dots_btn_x && x < in_dots_btn_x + in_dots_btn_w &&
        bar_y >= in_dots_btn_y && bar_y < in_dots_btn_y + in_dots_btn_h) {
      app.context_menu_open = true;
      app.context_menu_x = in_dots_btn_x;
      app.context_menu_y = in_dots_btn_y + in_dots_btn_h;
      app.context_menu_hover = -1; app.context_menu_hover_prev = -1; app.context_menu_sub_hover = -1;
      app.context_menu_file_idx = -5;
      app.context_menu_items = {
        AppState::menu_item(AppState::ContextMenuAction::NewFolder, "New Folder"),
        AppState::menu_item(AppState::ContextMenuAction::NewDocument, "New Document"),
        AppState::menu_item(AppState::ContextMenuAction::OpenWith, "Open With\u2026"),
        AppState::menu_item(AppState::ContextMenuAction::Separator, ""),
        AppState::menu_item(AppState::ContextMenuAction::Reload, "Reload"),
        AppState::menu_item(AppState::ContextMenuAction::CopyLocation, "Copy Location"),
        AppState::menu_item(AppState::ContextMenuAction::Separator, ""),
        AppState::menu_item(AppState::ContextMenuAction::Paste, "Paste"),
        AppState::menu_item(AppState::ContextMenuAction::SelectAll, "Select All"),
        AppState::menu_item(AppState::ContextMenuAction::InvertSelection, "Invert Selection"),
        AppState::menu_item(AppState::ContextMenuAction::SelectPattern, "Select by Pattern\u2026"),
        AppState::menu_item(AppState::ContextMenuAction::Separator, ""),
        AppState::menu_item(AppState::ContextMenuAction::OpenInTerminal, "Open in Terminal"),
        AppState::menu_item(AppState::ContextMenuAction::Separator, ""),
        AppState::menu_item(AppState::ContextMenuAction::Properties, "Properties"),
      };
      insert_template_submenu(app, 2);
      draw(app);
      return true;
    }

    // Navigation arrows (back, forward, up)
    int bx = app.nav_origin_x();
    int btn_w = static_cast<int>(36.0 * zf);
    int gap4 = static_cast<int>(6.0 * zf);
    // Sidebar fold toggle (drawn before the arrows when folded)
    if (sidebar_toggle_hit(app, x, y)) {
      toggle_sidebar_flap(app);
      draw(app);
      return true;
    }
    if (x >= bx && x < bx + btn_w && !app.cur_tab().nav_history.empty()) {
      navigate_back(app);
      draw(app);
      return true;
    }
    bx += btn_w + gap4;
    if (x >= bx && x < bx + btn_w && !app.cur_tab().nav_forward.empty()) {
      navigate_forward(app);
      draw(app);
      return true;
    }
    bx += btn_w + gap4;
    if (x >= bx && x < bx + btn_w && can_navigate_up(app)) {
      navigate_up(app);
      draw(app);
      return true;
    }

    // Folder-search button (folder + magnifying glass) → recursive search in current dir
    if (x >= in_folder_search_btn_x && x < in_folder_search_btn_x + in_folder_search_btn_w) {
    if (in_search_active) {
      reset_search_filters(app);
      in_search_active = false;
      in_recursive_search_active = false;
      in_search_query.clear();
      in_recursive_search_query.clear();
      recursive_search_worker().cancel();
      reload_dir(app);
    } else {
      in_recursive_search_active = false;
      in_search_active = true;
      in_search_query.clear();
      in_recursive_search_query.clear();
      recursive_search_worker().cancel();
      in_search_cursor = 0;
      in_search_sel_start = -1;
      in_search_sel_end = -1;
    }
    (app.active_pane ? app.r_path_editing : app.path_editing) = false;
    draw(app);
    return true;
  }
  // Search button (magnifying glass) → recursive home search
    if (x >= in_search_btn_x && x < in_search_btn_x + in_search_btn_w) {
    if (in_recursive_search_active) {
      reset_search_filters(app);
      in_recursive_search_active = false;
      in_search_active = false;
      in_search_query.clear();
      in_recursive_search_query.clear();
      recursive_search_worker().cancel();
      reload_dir(app);
    } else {
      in_search_active = false;
      in_recursive_search_active = true;
        in_search_query.clear();
        in_recursive_search_query.clear();
        recursive_search_worker().cancel();
        in_search_cursor = 0;
        in_search_sel_start = -1;
        in_search_sel_end = -1;
      }
      (app.active_pane ? app.r_path_editing : app.path_editing) = false;
      draw(app);
      return true;
    }

    // View-mode toggle (cycles through List → Grid → Compact → Tree → List)
    if (x >= in_view_btn_x && x < in_view_btn_x + in_view_btn_w) {
      auto cur = app.cur_tab().view_mode;
      if (cur == ViewMode::List) app.cur_tab().view_mode = ViewMode::Grid;
      else if (cur == ViewMode::Grid) app.cur_tab().view_mode = ViewMode::Compact;
      else if (cur == ViewMode::Compact) app.cur_tab().view_mode = ViewMode::Tree;
      else app.cur_tab().view_mode = ViewMode::List;
      app.last_browser_view_mode = app.cur_tab().view_mode;
      save_file_browser_settings(app);
      draw(app);
      return true;
    }
    if (x >= in_sort_btn_x && x < in_sort_btn_x + in_sort_btn_w) {
      // Close sort menu in both panes, then toggle active pane only
      bool was_open = app.active_pane ? app.r_sort_menu_open : app.sort_menu_open;
      app.r_sort_menu_open = false;
      app.sort_menu_open = false;
      if (!was_open)
        (app.active_pane ? app.r_sort_menu_open : app.sort_menu_open) = true;
      (app.active_pane ? app.r_sort_menu_hover : app.sort_menu_hover) = -1;
      (app.active_pane ? app.r_sort_menu_scroll : app.sort_menu_scroll) = 0;
      draw(app);
      return true;
    }

    // Filter button click (only when search is active)
    if (in_search_active || in_recursive_search_active) {
      if (x >= in_filter_btn_x && x < in_filter_btn_x + in_filter_btn_w) {
        auto& click_filter_section = app.active_pane ? app.r_filter_dropdown_section : app.filter_dropdown_section;
        auto& click_filter_hover = app.active_pane ? app.r_filter_dropdown_hover : app.filter_dropdown_hover;
        click_filter_section = (click_filter_section > 0) ? 0 : 1;
        click_filter_hover = -1;
        draw(app);
        return true;
      }
    }

    // Search bar click — set cursor position or clear
    if (in_search_active || in_recursive_search_active) {
      // Query-mode segment / case / lock hit tests
      {
        auto& cb_mode = app.active_pane ? app.r_search_mode : app.search_mode;
        auto& cb_case = app.active_pane ? app.r_search_case_sensitive : app.search_case_sensitive;
        auto& cb_lock = app.active_pane ? app.r_search_locked : app.search_locked;
        auto& cb_mode_x = app.active_pane ? app.r_search_mode_x : app.search_mode_x;
        auto& cb_mode_w = app.active_pane ? app.r_search_mode_w : app.search_mode_w;
        auto& cb_case_x = app.active_pane ? app.r_search_case_x : app.search_case_x;
        auto& cb_case_w = app.active_pane ? app.r_search_case_w : app.search_case_w;
        auto& cb_lock_x = app.active_pane ? app.r_search_lock_x : app.search_lock_x;
        auto& cb_lock_w = app.active_pane ? app.r_search_lock_w : app.search_lock_w;

        if (cb_mode_w > 0 && x >= cb_mode_x && x < cb_mode_x + cb_mode_w) {
          int seg = (x - cb_mode_x) / (cb_mode_w / 4);
          if (seg >= 0 && seg < 4 && cb_mode != seg) {
            cb_mode = seg;
            restart_active_search(app);
            draw(app);
          }
          return true;
        }
        if (cb_case_w > 0 && x >= cb_case_x && x < cb_case_x + cb_case_w) {
          cb_case = !cb_case;
          restart_active_search(app);
          draw(app);
          return true;
        }
        if (cb_lock_w > 0 && x >= cb_lock_x && x < cb_lock_x + cb_lock_w) {
          cb_lock = !cb_lock;
          draw(app);
          return true;
        }
      }

      // Clear button hit test
      if (in_search_clear_w > 0 && x >= in_search_clear_x && x < in_search_clear_x + in_search_clear_w) {
        in_search_query.clear();
        in_search_cursor = 0;
        in_search_sel_start = -1;
        in_search_sel_end = -1;
        recursive_search_worker().cancel();
        reload_dir(app);
        draw(app);
        return true;
      }
      // Click in search bar area — set cursor position
      if (x >= in_search_bar_x && x < in_search_bar_x + in_search_bar_w) {
        double zf = app.zoom_pct / 100.0;
        int search_icon_size = static_cast<int>(14.0 * zf);
        int text_left = in_search_bar_x + search_icon_size + static_cast<int>(8.0 * zf);
        int click_x = x - text_left;
        int best_pos = static_cast<int>(in_search_query.size());
        cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* cr_tmp = cairo_create(tmp);
        cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr_tmp, 13.0 * zf);
        for (int ci = 0; ci <= static_cast<int>(in_search_query.size()); ++ci) {
          std::string sub = in_search_query.substr(0, static_cast<std::size_t>(ci));
          cairo_text_extents_t te;
          cairo_text_extents(cr_tmp, sub.c_str(), &te);
          if (te.width >= click_x) { best_pos = ci; break; }
        }
        cairo_destroy(cr_tmp);
        cairo_surface_destroy(tmp);
        in_search_cursor = best_pos;
        in_search_sel_start = -1;
        in_search_sel_end = -1;
        draw(app);
        return true;
      }
    }

    // Search results banner — Clear button
    if (app.search_banner_clear_w > 0 &&
        (app.search_active || app.recursive_search_active ||
         app.r_search_active || app.r_recursive_search_active)) {
      int banner_y = app.top_bar_height + app.tab_bar_height;
      if (x >= app.search_banner_clear_x &&
          x < app.search_banner_clear_x + app.search_banner_clear_w &&
          y >= banner_y && y < banner_y + 28) {
        reset_search_filters(app);
        bool was_rec = app.active_pane ? app.r_recursive_search_active
                                       : app.recursive_search_active;
        auto& b_q = app.active_pane ? app.r_search_query : app.search_query;
        auto& b_rq = app.active_pane ? app.r_recursive_search_query : app.recursive_search_query;
        b_q.clear();
        b_rq.clear();
        if (app.active_pane) { app.r_search_active = false; app.r_recursive_search_active = false; }
        else { app.search_active = false; app.recursive_search_active = false; }
        recursive_search_worker().cancel();
        reload_dir(app);
        draw(app);
        return true;
      }
    }

    // Path editing click — set cursor position by character hit-test
    if (app.active_pane ? app.r_path_editing : app.path_editing) {
      auto& pe_buf = app.active_pane ? app.r_path_edit_buf : app.path_edit_buf;
      auto& pe_cursor = app.active_pane ? app.r_path_edit_cursor : app.path_edit_cursor;
      auto& pe_sel_start = app.active_pane ? app.r_path_edit_sel_start : app.path_edit_sel_start;
      auto& pe_sel_end = app.active_pane ? app.r_path_edit_sel_end : app.path_edit_sel_end;
      auto& pe_dragging = app.active_pane ? app.r_path_edit_dragging : app.path_edit_dragging;
      double zf = app.zoom_pct / 100.0;
      int arrow_w = static_cast<int>(36.0 * zf);
      int gap4 = static_cast<int>(6.0 * zf);
      int mx6 = static_cast<int>(24.0 * zf);
      int path_pad = static_cast<int>(12.0 * zf);
      int house_w = static_cast<int>(16.0 * zf);
      int gap12 = static_cast<int>(12.0 * zf);
      int nav_origin = app.nav_origin_x();
      int path_x_inner = nav_origin + 3 * arrow_w + 2 * gap4 + mx6 + path_pad + house_w + gap12;
      int path_w_inner = in_search_btn_x - static_cast<int>(6.0 * zf) - path_x_inner;
      int text_x = path_x_inner;
      int field_right = path_x_inner + path_w_inner - static_cast<int>(14.0 * zf);
      if (x >= path_x_inner && x < field_right) {
        cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* cr_tmp = cairo_create(tmp);
        cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr_tmp, 13.0 * zf);
        const std::string& buf = pe_buf;
        cairo_text_extents_t full_te;
        cairo_text_extents(cr_tmp, buf.c_str(), &full_te);
        int scroll_offset = 0;
        std::string display = buf;
        if (full_te.width > path_w_inner - static_cast<int>(20.0 * zf)) {
          int keep = static_cast<int>(display.size()) * (path_w_inner - static_cast<int>(20.0 * zf)) /
                    std::max(1, static_cast<int>(full_te.width));
          if (keep > 3 && keep < static_cast<int>(display.size())) {
            int trim = static_cast<int>(display.size()) - keep + 3;
            scroll_offset = trim;
            display = "..." + display.substr(static_cast<std::size_t>(trim));
          }
        }
        int click_x = x - text_x;
        int best_pos = static_cast<int>(buf.size());
        for (int ci = 0; ci <= static_cast<int>(display.size()); ++ci) {
          std::string sub = display.substr(0, static_cast<std::size_t>(ci));
          cairo_text_extents_t te;
          cairo_text_extents(cr_tmp, sub.c_str(), &te);
          if (te.width >= click_x) {
            if (scroll_offset > 0) {
              best_pos = (ci <= 3) ? scroll_offset : scroll_offset + ci - 3;
            } else {
              best_pos = ci;
            }
            break;
          }
        }
        cairo_destroy(cr_tmp);
        cairo_surface_destroy(tmp);
        pe_cursor = best_pos;
        pe_sel_start = -1;
        pe_sel_end = -1;
        pe_dragging = true;
        draw(app);
      } else {
        pe_cursor = static_cast<int>(pe_buf.size());
        pe_sel_start = -1;
        pe_sel_end = -1;
        pe_dragging = true;
        draw(app);
      }
      return true;
    }

    // Breadcrumb click
    for (size_t i = 0; i < in_breadcrumbs.size(); ++i) {
      auto& seg = in_breadcrumbs[i];
      if (x >= seg.x && x < seg.x + seg.w) {
        navigate_to(app, seg.path);
        draw(app);
        return true;
      }
    }
    // Click on empty area in top bar → enter path editing mode
    (app.active_pane ? app.r_path_edit_buf : app.path_edit_buf) = app.cur_tab().current_path;
    (app.active_pane ? app.r_path_editing : app.path_editing) = true;
    {
      double zf = app.zoom_pct / 100.0;
      int arrow_w = static_cast<int>(36.0 * zf);
      int gap4 = static_cast<int>(6.0 * zf);
      int mx6 = static_cast<int>(24.0 * zf);
      int path_pad = static_cast<int>(12.0 * zf);
      int house_w = static_cast<int>(16.0 * zf);
      int gap12 = static_cast<int>(12.0 * zf);
      int nav_origin = app.nav_origin_x();
      int path_x = nav_origin + 3 * arrow_w + 2 * gap4 + mx6 + path_pad + house_w + gap12;
int path_w = in_search_btn_x - static_cast<int>(6.0 * zf) - path_x;
      int text_x = path_x;
      cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t* cr_tmp = cairo_create(tmp);
      cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr_tmp, 13.0 * zf);
      const std::string& buf = (app.active_pane ? app.r_path_edit_buf : app.path_edit_buf);
      cairo_text_extents_t full_te;
      cairo_text_extents(cr_tmp, buf.c_str(), &full_te);
      int scroll_offset = 0;
      std::string display = buf;
      if (full_te.width > path_w - static_cast<int>(20.0 * zf)) {
        int keep = static_cast<int>(display.size()) * (path_w - static_cast<int>(20.0 * zf)) /
                  std::max(1, static_cast<int>(full_te.width));
        if (keep > 3 && keep < static_cast<int>(display.size())) {
          int trim = static_cast<int>(display.size()) - keep + 3;
          scroll_offset = trim;
          display = "..." + display.substr(static_cast<std::size_t>(trim));
        }
      }
      int click_x = x - text_x;
      int best_pos = static_cast<int>(buf.size());
      for (int ci = 0; ci <= static_cast<int>(display.size()); ++ci) {
        std::string sub = display.substr(0, static_cast<std::size_t>(ci));
        cairo_text_extents_t te;
        cairo_text_extents(cr_tmp, sub.c_str(), &te);
        if (te.width >= click_x) {
          if (scroll_offset > 0) {
            best_pos = (ci <= 3) ? scroll_offset : scroll_offset + ci - 3;
          } else {
            best_pos = ci;
          }
          break;
        }
      }
      cairo_destroy(cr_tmp);
      cairo_surface_destroy(tmp);
      (app.active_pane ? app.r_path_edit_cursor : app.path_edit_cursor) = best_pos;
    }
    (app.active_pane ? app.r_path_edit_sel_start : app.path_edit_sel_start) = -1;
    (app.active_pane ? app.r_path_edit_sel_end : app.path_edit_sel_end) = -1;
    (app.active_pane ? app.r_path_edit_dragging : app.path_edit_dragging) = true;
    draw(app);
    return true;
  }
  return false;
}

bool click_tab_bar(AppState& app, int x, int y, int button) {
  // ── Tab bar click ──
  if (y >= app.top_bar_height && y < app.top_bar_height + app.tab_bar_height) {
    app.last_click_ns = 0;
    for (size_t i = 0; i < app.tab_hits.size(); ++i) {
      auto& hit = app.tab_hits[i];
      if (x >= hit.x && x < hit.x + hit.w) {
        if (button == 0x110 && hit.close_x > 0 && x >= hit.close_x) {
          // Left-click on close button
          if (i < app.tabs.size()) {
            app.active_tab = static_cast<int>(i);
            close_tab(app);
            draw(app);
            return true;
          }
        }
        if (button == 0x110 && i < app.tabs.size()) {
          // Left-click on tab — set up drag potential
          app.tab_drag_from = static_cast<int>(i);
          app.tab_drag_start_x = x;
          if (static_cast<int>(i) != app.active_tab ||
              (app.split_view && app.active_pane == 1)) {
            open_tab_in_active_pane(app, static_cast<int>(i));
          }
          draw(app);
          return true;
        }
        if (button == 0x210 && i < app.tabs.size()) {
          // Middle-click on tab → close
          app.active_tab = static_cast<int>(i);
          close_tab(app);
          draw(app);
          return true;
        }
        if (button == 0x210 && i < app.tabs.size()) {
          // Middle-click on tab → close (only reached for left-clicks due to outer scope)
          app.active_tab = static_cast<int>(i);
          close_tab(app);
          draw(app);
          return true;
        }
      }
    }
    return true;
  }
  return false;
}

bool click_ops_cancel(AppState& app, int x, int y, int button) {
  // ── Operations panel cancel button ──
  if (app.ops_panel_open && app.ops_cancel_w > 0) {
    if (x >= app.ops_cancel_x && x < app.ops_cancel_x + app.ops_cancel_w &&
        y >= app.ops_cancel_y && y < app.ops_cancel_y + app.ops_cancel_h) {
      if (app.op_progress) app.op_progress->cancel = true;
      app.operation_status = "Cancelling...";
      app.operation_status_expires_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count() + 3000;
      draw(app);
      return true;
    }
  }
  return false;
}

bool click_flap_swallow(AppState& app, int x, int y, int button) {
  // ── Flap swallow ──
  // Inside the revealed overlay but not on a sidebar item: swallow the click
  // so it doesn't act on the content hidden beneath the flap.
  if (app.sidebar_folded && app.sidebar_folded_revealed && button == 0x110 &&
      x < app.effective_sidebar_width() && y >= app.content_top_y()) {
    draw(app);
    return true;
  }
  return false;
}

bool click_column_header(AppState& app, int x, int y, int button) {
  // ── Column header click (list view) ──
  if (app.cur_tab().view_mode == ViewMode::List && y >= app.top_bar_height + app.tab_bar_height &&
      y < app.top_bar_height + app.tab_bar_height + app.entry_height) {
    // Right-click → column chooser
    if (button == 0x111) {
      auto& cmo = app.active_pane ? app.r_columns_menu_open : app.columns_menu_open;
      auto& cmx = app.active_pane ? app.r_columns_menu_x : app.columns_menu_x;
      auto& cmy = app.active_pane ? app.r_columns_menu_y : app.columns_menu_y;
      app.r_sort_menu_open = false;
      app.sort_menu_open = false;
      cmo = true;
      cmx = x;
      cmy = y;
      (app.active_pane ? app.r_columns_menu_hover : app.columns_menu_hover) = -1;
      draw(app);
      return true;
    }
    int s_w = app.sidebar_w();
    double zf = app.zoom_pct / 100.0;
    int text_x = s_w + static_cast<int>(28.0 * zf);
    int content_w = app.width - s_w;
    int name_w = static_cast<int>(content_w * app.col_name_frac);
    int size_w = static_cast<int>(content_w * app.col_size_frac);
    int date_w = static_cast<int>(content_w * app.col_date_frac);
    int x1 = text_x + name_w;
    int x2 = x1 + size_w;
    int x3 = x2 + date_w;

    if (std::abs(x - x1) < 4) {
      app.col_resizing = 0;
      app.col_resize_start_frac = static_cast<double>(x - s_w) /
                                   static_cast<double>(std::max(1, content_w));
      draw(app); return true;
    }
    if (std::abs(x - x2) < 4) {
      app.col_resizing = 1;
      app.col_resize_start_frac = static_cast<double>(x - s_w) /
                                   static_cast<double>(std::max(1, content_w));
      draw(app); return true;
    }
    if (std::abs(x - x3) < 4) {
      app.col_resizing = 2;
      app.col_resize_start_frac = static_cast<double>(x - s_w) /
                                   static_cast<double>(std::max(1, content_w));
      draw(app); return true;
    }

    SortField clicked = SortField::Name;
    if (x >= text_x && x < x1) clicked = SortField::Name;
    else if (x >= x1 && x < x2) clicked = SortField::Size;
    else if (x >= x2 && x < x3) clicked = SortField::Modified;
    else if (x >= x3) clicked = SortField::Type;
    else { draw(app); return true; }

    if (app.cur_tab().sort_field == clicked)
      app.cur_tab().sort_descending = !app.cur_tab().sort_descending;
    else {
      app.cur_tab().sort_field = clicked;
      app.cur_tab().sort_descending = false;
    }
    reload_dir(app);
    draw(app);
    return true;
  }
  return false;
}

} // namespace eh::file_browser
