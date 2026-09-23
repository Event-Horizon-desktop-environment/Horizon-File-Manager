// click_views.cpp — views split from click.cpp (handle_click region carve).
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

bool click_split_pane(AppState& app, int x, int y, int button) {
  // Split pane: determine which pane was clicked
  if (app.split_view) {
    int s_w = app.sidebar_w();
    int content_w = app.width - s_w - (app.info_panel_open ? app.info_panel_width : 0);
    int split = app.split_divider_x;
    if (split <= 0) split = content_w / 2;
    int div_x = s_w + split;
    int div_w = 4;
    if (x >= div_x && x < div_x + div_w) {
      if (button == 0x110) { app.split_divider_dragging = true; app.split_divider_hover = true; }
      return true;
    }
    app.active_pane = (x >= div_x + div_w) ? 1 : 0;
  }
  return false;
}

bool click_sidebar_hit(AppState& app, int x, int y, int button) {
  int sb_idx = hit_test_sidebar(app, x, y);
  if (sb_idx >= 0 && sb_idx < static_cast<int>(app.sidebar_locations.size())) {
    app.last_click_ns = 0;
    app.sidebar_hover_idx = sb_idx;
    auto& loc = app.sidebar_locations[sb_idx];
    if (loc.kind == SidebarLocation::Kind::Computer) {
      if (app.cur_tab().view_mode != ViewMode::Computer)
        app.last_browser_view_mode = app.cur_tab().view_mode;
      app.cur_tab().view_mode = ViewMode::Computer;
      app.cur_tab().current_path = "computer://";
      app.cur_tab().selected_idx = -1;
      app.cur_tab().hover_idx = -1;
      app.cur_tab().scroll_px = 0;
      app.computer_scroll_px = 0;
      app.computer_scroll_smooth_current = 0;
      app.computer_scroll_smooth_target = 0;
      app.computer_needs_refresh = true;
      app.sidebar_folded_revealed = false;
      draw(app);
      return true;
    } else if (loc.kind == SidebarLocation::Kind::Drive && !loc.drive_id.empty()) {
      if (loc.is_mounted) {
        double zf = app.zoom_pct / 100.0;
        int icon_left = app.effective_sidebar_width() - static_cast<int>(22.0 * zf);
        // Only unmount when clicking the mount indicator icon (right side of the item)
        if (x >= icon_left) {
          unmount_drive(app, sb_idx);
        } else {
          navigate_to(app, loc.path);
          app.sidebar_folded_revealed = false;
        }
      } else {
        mount_drive(app, sb_idx);
      }
    } else {
      navigate_to(app, loc.path);
      // Keep the flap open for favorites so reorder drags still work
      if (loc.kind != SidebarLocation::Kind::Favorite)
        app.sidebar_folded_revealed = false;
    }
    // Set up potential drag for reordering favorites
    if (loc.kind == SidebarLocation::Kind::Favorite) {
      // Compute fav index from sidebar location index
      int places_end = 0;
      while (places_end < static_cast<int>(app.sidebar_locations.size()) &&
             app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Favorite &&
             app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Root &&
             app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Drive)
        ++places_end;
      int fav_idx = sb_idx - places_end;
      if (fav_idx >= 0 && fav_idx < static_cast<int>(app.favorites.size())) {
        app.sidebar_fav_dragging = false;
        app.sidebar_fav_drag_from = fav_idx;
        app.sidebar_fav_drag_start_y = y;
        app.sidebar_fav_drag_to = -1;
      }
    }
    draw(app);
    return true;
  }
  return false;
}

bool click_content_hit(AppState& app, int x, int y, int button, uint64_t now_ns) {
  // ── Content-area hit-test ──
  int idx = -1;
  if (app.cur_tab().view_mode == ViewMode::List) {
    idx = hit_test_list(app, x, y);
  } else if (app.cur_tab().view_mode == ViewMode::Grid) {
    idx = hit_test_grid(app, x, y);
  } else if (app.cur_tab().view_mode == ViewMode::Computer) {
    idx = hit_test_computer(app, x, y);
  } else if (app.cur_tab().view_mode == ViewMode::Tree) {
    idx = hit_test_tree(app, x, y, true);
  } else if (app.cur_tab().view_mode == ViewMode::Compact) {
    idx = hit_test_compact(app, x, y);
  }

  if (idx == -2) {
    // Tree view arrow toggle
    build_tree_entries(app);
    draw(app);
    return true;
  }

  if (idx >= 0) {
    // Mouse selection: clear the keyboard-selection flag so hover
    // preview/tooltip don't chase a merely-click-selected item.
    app.cur_tab().selected_by_kbd = false;
    // Computer view: single click selects/mounts, double click opens
    if (app.cur_tab().view_mode == ViewMode::Computer) {
      auto& item = app.computer_items[idx];
      // Single-click mount for unmounted drives (matches sidebar behavior)
      if (item.shape == ComputerItem::ShapeType::Large && !item.is_mounted && !item.drive_id.empty()) {
        for (size_t si = 0; si < app.sidebar_locations.size(); ++si) {
          auto& sloc = app.sidebar_locations[si];
          if (sloc.kind == SidebarLocation::Kind::Drive && sloc.drive_id == item.drive_id) {
            mount_drive(app, static_cast<int>(si));
            break;
          }
        }
        app.computer_hover_idx = idx;
        app.last_click_ns = now_ns;
        app.last_click_x = x;
        app.last_click_y = y;
        app.last_click_idx = idx;
        draw(app);
        return true;
      }
      uint64_t elapsed_ns = now_ns - app.last_click_ns;
      bool same_pos = std::abs(x - app.last_click_x) < 8 &&
                      std::abs(y - app.last_click_y) < 8;
      if (idx >= 0 && idx == app.last_click_idx && same_pos && elapsed_ns < 400000000ull) {
        // Double-click
        if (item.shape == ComputerItem::ShapeType::Small && !item.path.empty()) {
          navigate_to(app, item.path);
          app.last_click_ns = 0;
          return true;
        } else if (item.shape == ComputerItem::ShapeType::Large && item.is_mounted && !item.path.empty()) {
          navigate_to(app, item.path);
          app.last_click_ns = 0;
          return true;
        }
        app.last_click_ns = 0;
        draw(app);
        return true;
      }
      // Single click: select
      app.computer_hover_idx = idx;
      app.last_click_ns = now_ns;
      app.last_click_x = x;
      app.last_click_y = y;
      app.last_click_idx = idx;
      draw(app);
      return true;
    }

    auto* xkb = app.seat.xkb_state_ptr();
    bool ctrl_mod = xkb && xkb_state_mod_name_is_active(xkb, XKB_MOD_NAME_CTRL,
                                                         XKB_STATE_MODS_EFFECTIVE) != 0;
    bool shift_mod = xkb && xkb_state_mod_name_is_active(xkb, XKB_MOD_NAME_SHIFT,
                                                           XKB_STATE_MODS_EFFECTIVE) != 0;

    uint64_t elapsed_ns = now_ns - app.last_click_ns;
    bool same_pos = std::abs(x - app.last_click_x) < 8 &&
                    std::abs(y - app.last_click_y) < 8;
    if (idx == app.last_click_idx && same_pos && elapsed_ns < 400000000ull) {
      app.cur_tab().selected_idx = idx;
      app.cur_tab().multi_selected = {idx};
      app.drag_potential = false;
      app.drag_potential_idx = -1;
      open_selected(app);
      app.last_click_ns = 0;
      draw(app);
      return true;
    }

    if (shift_mod && !ctrl_mod) {
      // Shift-click: select range from anchor to idx
      if (app.cur_tab().sel_anchor < 0) app.cur_tab().sel_anchor = 0;
      int lo = std::min(app.cur_tab().sel_anchor, idx);
      int hi = std::max(app.cur_tab().sel_anchor, idx);
      app.cur_tab().multi_selected.clear();
      for (int i = lo; i <= hi; ++i) app.cur_tab().multi_selected.push_back(i);
      app.cur_tab().selected_idx = idx;
    } else if (ctrl_mod && !shift_mod) {
      // Ctrl-click: toggle idx in multi_selected
      auto it = std::find(app.cur_tab().multi_selected.begin(), app.cur_tab().multi_selected.end(), idx);
      if (it != app.cur_tab().multi_selected.end()) {
        app.cur_tab().multi_selected.erase(it);
        // If selected_idx was this item, pick another or -1
        if (app.cur_tab().selected_idx == idx) {
          app.cur_tab().selected_idx = app.cur_tab().multi_selected.empty() ? -1 : app.cur_tab().multi_selected.back();
        }
      } else {
        app.cur_tab().multi_selected.push_back(idx);
        app.cur_tab().selected_idx = idx;
      }
      app.cur_tab().sel_anchor = idx;
    } else {
      // Plain click: single select — but preserve multi-selection if
      // clicking an already-selected file (so drag picks up all items).
      auto it = std::find(app.cur_tab().multi_selected.begin(),
                          app.cur_tab().multi_selected.end(), idx);
      if (it != app.cur_tab().multi_selected.end()) {
        // Already selected — keep multi_selected intact, just update anchor
        app.cut_paths.clear();
        app.cur_tab().selected_idx = idx;
        app.cur_tab().sel_anchor = idx;
      } else {
        app.cut_paths.clear();
        app.cur_tab().selected_idx = idx;
        app.cur_tab().multi_selected = {idx};
        app.cur_tab().sel_anchor = idx;
      }
    }
    app.last_click_ns = now_ns;
    app.last_click_x = x;
    app.last_click_y = y;
    app.last_click_idx = idx;

    // Tree rows: remember WHICH row is selected by path — tree indices
    // shift as folders expand/collapse, and child rows have no slot in
    // visible_entries.
    if (app.cur_tab().view_mode == ViewMode::Tree) {
      if (idx >= 0 && idx < static_cast<int>(app.cur_tab().tree_entries.size()))
        app.cur_tab().tree_selected_path = app.cur_tab().tree_entries[idx].path;
      else
        app.cur_tab().tree_selected_path.clear();
    }

    // Set drag potential (only for plain click)
    if (!shift_mod && !ctrl_mod) {
      app.drag_potential = true;
      app.drag_potential_idx = idx;
      app.drag_start_x = static_cast<double>(x);
      app.drag_start_y = static_cast<double>(y);
      app.drag_button_serial = app.seat.last_pointer_button_serial();
      app.drag_paths.clear();
      for (int vis_idx : app.cur_tab().multi_selected) {
        if (vis_idx >= 0 && vis_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
          int real_idx = app.cur_tab().visible_entries[vis_idx];
          if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()))
            app.drag_paths.push_back(app.cur_tab().entries[real_idx].path);
        }
      }
    }
  } else {
    // Start marquee selection on empty background
    app.marquee_x0 = static_cast<double>(x);
    app.marquee_y0 = static_cast<double>(y);
    app.marquee_x1 = static_cast<double>(x);
    app.marquee_y1 = static_cast<double>(y);
    app.marquee_active = true;
    app.cur_tab().selected_idx = -1;
    app.cur_tab().multi_selected.clear();
    app.cur_tab().sel_anchor = -1;
    app.last_click_ns = 0;
  }
  draw(app);
  return true;
}

bool click_rpath_edit(AppState& app, int x, int y, int button) {
  if ((app.active_pane ? app.r_path_editing : app.path_editing)) {
    int rc_bar_y = y;
    if (app.split_view) {
      int content_y = app.top_bar_height + app.tab_bar_height;
      if (y >= content_y && y < content_y + app.top_bar_height)
        rc_bar_y = y - content_y;
    }
    if (rc_bar_y < app.top_bar_height) {
    app.context_menu_open = true;
    app.context_menu_x = x;
    app.context_menu_y = y;
    app.context_menu_hover = -1; app.context_menu_hover_prev = -1; app.context_menu_sub_hover = -1;
    app.context_menu_file_idx = -3; // path editing
    app.context_menu_sidebar_idx = -1;
    app.context_menu_items = {
      AppState::menu_item(AppState::ContextMenuAction::Copy, "Copy"),
    };
    draw(app);
    return true;
  }
  }
  return false;
}

bool click_rtab_bar(AppState& app, int x, int y, int button) {
  if (y >= app.top_bar_height && y < app.top_bar_height + app.tab_bar_height) {
    for (size_t i = 0; i < app.tab_hits.size(); ++i) {
      auto& hit = app.tab_hits[i];
      if (x >= hit.x && x < hit.x + hit.w && i < app.tabs.size()) {
        app.context_menu_open = true;
        app.context_menu_x = x;
        app.context_menu_y = y;
        app.context_menu_hover = -1; app.context_menu_hover_prev = -1; app.context_menu_sub_hover = -1;
        app.context_menu_file_idx = -4;
        app.context_menu_tab_idx = static_cast<int>(i);
        app.context_menu_items = {};
        if (!app.closed_tabs.empty())
          app.context_menu_items.push_back(
            AppState::menu_item(AppState::ContextMenuAction::ReopenClosedTab, "Reopen Closed Tab"));
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::CloseTab, "Close Tab"));
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::CloseOtherTabs, "Close Other Tabs"));
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::CloseAllTabs, "Close All Tabs"));
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::DuplicateTab, "Duplicate Tab"));
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::ToggleSplitView,
                              app.split_view ? "Exit Split View" : "Split View"));
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::Separator, ""));
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::OpenInNewWindow, "Open in new window"));
        draw(app);
        return true;
      }
    }
    return true;
  }
  return false;
}

bool click_rcomputer(AppState& app, int x, int y, int button) {
  if (app.cur_tab().view_mode == ViewMode::Computer) {
    int cidx = hit_test_computer(app, x, y);
    if (cidx >= 0 && cidx < static_cast<int>(app.computer_items.size())) {
      auto& citem = app.computer_items[cidx];
      if (citem.shape == ComputerItem::ShapeType::Large && !citem.drive_id.empty()) {
        app.context_menu_open = true;
        app.context_menu_x = x;
        app.context_menu_y = y;
        app.context_menu_hover = -1; app.context_menu_hover_prev = -1; app.context_menu_sub_hover = -1;
        app.context_menu_file_idx = cidx;
        app.context_menu_items = {};
        // Find matching sidebar item for mount_drive/unmount_drive
        app.context_menu_sidebar_idx = -1;
        for (size_t si = 0; si < app.sidebar_locations.size(); ++si) {
          if (app.sidebar_locations[si].kind == SidebarLocation::Kind::Drive &&
              app.sidebar_locations[si].drive_id == citem.drive_id) {
            app.context_menu_sidebar_idx = static_cast<int>(si);
            break;
          }
        }
        if (citem.is_mounted)
          app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::UnmountDrive, "Unmount"));
        else
          app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::MountDrive, "Mount"));
        draw(app);
        return true;
      }
    }
  }
  return false;
}

bool click_rctx_close(AppState& app, int x, int y, int button) {
  if (app.context_menu_open) {
    app.context_menu_open = false;
    draw(app);
    return true;
  }
  return false;
}

bool click_rsidebar(AppState& app, int x, int y, int button) {
  int sb_idx = hit_test_sidebar(app, x, y);
  if (sb_idx >= 0 && sb_idx < static_cast<int>(app.sidebar_locations.size())) {
    auto& loc = app.sidebar_locations[sb_idx];
    app.context_menu_open = true;
    app.context_menu_x = x;
    app.context_menu_y = y;
    app.context_menu_hover = -1; app.context_menu_hover_prev = -1; app.context_menu_sub_hover = -1;
    app.context_menu_file_idx = -2; // sidebar item
    app.context_menu_sidebar_idx = sb_idx;
    app.context_menu_items = {};

    if (loc.kind == SidebarLocation::Kind::Favorite) {
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::RemoveFromFavorites, "Remove from Favorites"));
    } else if (loc.kind == SidebarLocation::Kind::Drive) {
      if (loc.is_mounted && !loc.drive_id.empty()) {
        app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::UnmountDrive, "Unmount"));
      }
      if (!loc.is_mounted && !loc.drive_id.empty()) {
        app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::MountDrive, "Mount"));
      }
    } else if (loc.kind == SidebarLocation::Kind::Trash) {
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Open, "Open"));
      app.context_menu_items.push_back(AppState::menu_separator());
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::EmptyTrash, "Empty Trash"));
      app.context_menu_items.push_back(AppState::menu_separator());
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::OpenInNewTab, "Open in new tab"));
    } else if (loc.kind == SidebarLocation::Kind::Computer) {
      // No context menu actions for computer view virtual path
    }

    if (!loc.path.empty() && loc.kind != SidebarLocation::Kind::Trash) {
      if (!app.context_menu_items.empty())
        app.context_menu_items.push_back(AppState::menu_separator());
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::OpenInNewTab, "Open in new tab"));
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::OpenInNewWindow, "Open in new window"));
    }

    draw(app);
    return true;
  }
  return false;
}

bool click_rcontent(AppState& app, int x, int y, int button) {
  int idx = -1;
  if (app.cur_tab().view_mode == ViewMode::List) {
    idx = hit_test_list(app, x, y);
  } else if (app.cur_tab().view_mode == ViewMode::Grid) {
    idx = hit_test_grid(app, x, y);
  } else if (app.cur_tab().view_mode == ViewMode::Computer) {
    idx = hit_test_computer(app, x, y);
  } else if (app.cur_tab().view_mode == ViewMode::Tree) {
    idx = hit_test_tree(app, x, y);
  } else if (app.cur_tab().view_mode == ViewMode::Compact) {
    idx = hit_test_compact(app, x, y);
  }

  open_context_menu(app, idx, x, y);
  draw(app);
  return true;
}

} // namespace eh::file_browser
