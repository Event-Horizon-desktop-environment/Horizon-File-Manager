// pointer.cpp — pointer-move handler (scrollbar thumb drag, sidebar
// fold flap hover, edge resize hover, hover preview). Moved from
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
// ── pointer-move handler (moved from events.cpp) ──────────────────────────
void handle_pointer_move(AppState& app, int x, int y) {
  app.pointerX = static_cast<double>(x);
  app.pointerY = static_cast<double>(y);

  // ── Status-bar zoom slider drag (relative: 1 level per ~7px) ──
  if (app.status_zoom_dragging && app.status_zoom_slider_w > 0) {
    double px_per_lvl = std::max(
        1.0, static_cast<double>(app.status_zoom_slider_w) / (kZoomLevelCount - 1));
    int lvl = std::clamp(
        app.status_zoom_press_level +
            static_cast<int>(std::lround(
                static_cast<double>(x - app.status_zoom_press_x) / px_per_lvl)),
        0, kZoomLevelCount - 1);
    if (lvl != app.status_zoom_last_level) {
      app.status_zoom_last_level = lvl;
      apply_zoom_pct(app, zoom_pct_for_level(lvl));
      draw(app);
    }
    return;
  }

  // ── Main-view scrollbar drag ──
  if (app.scrollbar_dragging) {
    apply_scrollbar_drag(app, y);
    draw(app);
    return;
  }

  // ── Split pane divider drag ──
  if (app.split_view) {
    int s_w = app.sidebar_w();
    int content_w = app.width - s_w - (app.info_panel_open ? app.info_panel_width : 0);
    int split = app.split_divider_x;
    if (split <= 0) split = content_w / 2;
    int div_x = s_w + split;
    int div_w = 4;
    if (app.split_divider_dragging) {
      app.split_divider_x = std::clamp(x - s_w, 100, app.width - s_w - 100 - app.info_panel_width);
      draw(app);
      return;
    }
    bool over_div = (x >= div_x && x < div_x + div_w);
    if (over_div != app.split_divider_hover) {
      app.split_divider_hover = over_div;
      draw(app);
      return;
    }
    // Set active pane based on pointer position (after divider checks)
    app.active_pane = (x >= div_x + div_w) ? 1 : 0;
  }

  // ── Marquee drag ──
  if (app.marquee_active) {
    app.marquee_x1 = static_cast<double>(x);
    app.marquee_y1 = static_cast<double>(y);
    hit_test_marquee(app);
    draw(app);
    return;
  }

  // ── Drag potential: initiate drag if past threshold ──
  if (app.drag_potential && !app.marquee_active) {
    double dx = x - app.drag_start_x;
    double dy = y - app.drag_start_y;
    if (dx * dx + dy * dy > 64.0) {
      if (!app.drag_paths.empty()) {
        start_drag(app);
      } else {
        cancel_drag(app);
      }
      if (app.wl.display()) wl_display_flush(app.wl.display());
      return;
    }
  }

  // ── Sidebar drag ──
  if (app.sidebar_dragging) {
    drag_sidebar_resize(app, x);
    draw(app);
    return;
  }

  // ── Sidebar favorite reorder drag ──
  if (app.sidebar_fav_drag_from >= 0 && !app.sidebar_fav_dragging) {
    int dy = y - app.sidebar_fav_drag_start_y;
    if (dy * dy > 64) {
      app.sidebar_fav_dragging = true;
    }
  }
  if (app.sidebar_fav_dragging && app.sidebar_expanded) {
    app.sidebar_fav_drag_current_y = y;
    // Recalculate places_end to map fav indices
    int total = static_cast<int>(app.sidebar_locations.size());
    int places_end = 0;
    while (places_end < total &&
           app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Favorite &&
           app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Root &&
           app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Drive)
      ++places_end;
    int fav_start = places_end;
    while (fav_start < total &&
           app.sidebar_locations[fav_start].kind == SidebarLocation::Kind::Favorite)
      ++fav_start;
    int fav_count = fav_start - places_end;

    // Insertion slot from painted row tops (retained registry) — the old
    // hand-derived offset disagreed with paint by ~72px.
    int slot = 0;
    for (int k = 0; k < fav_count; ++k) {
      const hui::HitRegion* r =
          app.hit_main.find_id(hui::Hit::sidebar_row(places_end + k));
      if (r == nullptr) continue;
      if (y < r->y) break;
      slot = k + 1;
    }
    if (slot > fav_count) slot = fav_count;

    int visual_to = slot;
    int new_to = slot;
    if (new_to > app.sidebar_fav_drag_from) {
      // Remove shifts items left → insertion index is one less
      new_to = new_to - 1;
    }

    if (new_to != app.sidebar_fav_drag_to)
      app.sidebar_fav_drag_to = new_to;
    app.sidebar_fav_drag_to_visual = visual_to;
    draw(app);
    return;
  }

  // ── Tab reorder drag ──
  if (app.tab_drag_from >= 0 && !app.tab_dragging) {
    int dx = x - app.tab_drag_start_x;
    if (dx * dx > 64) {
      app.tab_dragging = true;
      app.tab_drag_current_x = x;
    }
  }
  if (app.tab_dragging) {
    app.tab_drag_current_x = x;
    int n = static_cast<int>(app.tabs.size());
    // Determine slot based on mouse x relative to tab hit rects
    int slot = n;
    for (int i = 0; i < n; ++i) {
      auto& hit = app.tab_hits[i];
      int mid = hit.x + hit.w / 2;
      if (x < mid) {
        slot = i;
        break;
      }
    }
    int visual_to = slot;
    int new_to = slot;
    if (new_to > app.tab_drag_from) {
      new_to = new_to - 1;
    }
    if (new_to != app.tab_drag_to)
      app.tab_drag_to = new_to;
    app.tab_drag_to_visual = visual_to;
    draw(app);
    return;
  }

  // ── Sidebar resize edge hover ──
  if (update_sidebar_resize_hover(app, x)) {
    draw(app);
  }

  // ── Confirm dialog hover ──
  if (app.confirm_open) {
    const uint32_t hid = app.hit_main.query(x, y);
    int new_hover = -1;
    if (hid == hui::Hit::dialog(hui::Hit::kDlgConfirm, hui::Hit::kConfirmDelete))
      new_hover = 1;
    else if (hid == hui::Hit::dialog(hui::Hit::kDlgConfirm, hui::Hit::kConfirmCancel))
      new_hover = 0;
    if (new_hover != app.confirm_hover_btn) {
      app.confirm_hover_btn = new_hover;
      draw(app);
    }
    return;
  }

  // ── Compress dialog hover ──
  if (app.compress_dialog_open) {
    const uint32_t hid = app.hit_main.query(x, y);
    int new_hover_fmt = -1;
    int new_hover_lvl = -1;
    int new_hover_thr = -1;
    int new_hover_btn = -1;
    if ((hid & hui::Hit::kGroupMask) == hui::Hit::kDialog) {
      int ctrl = hui::Hit::dialog_ctrl(hid);
      if ((hid & 0xFFFFC00) == hui::Hit::dialog(hui::Hit::kDlgCompress, 0)) {
        if (ctrl >= hui::Hit::kCompressFormatBase && ctrl < hui::Hit::kCompressFormatBase + 7) {
          int i = ctrl - hui::Hit::kCompressFormatBase;
          if (i >= 0 && i < 7 && app.compress_format_available[i]) new_hover_fmt = i;
        } else if (ctrl >= hui::Hit::kCompressLevelBase && ctrl < hui::Hit::kCompressLevelBase + 5) {
          new_hover_lvl = ctrl - hui::Hit::kCompressLevelBase;
        } else if (ctrl >= hui::Hit::kCompressThreadBase) {
          new_hover_thr = ctrl - hui::Hit::kCompressThreadBase;
        }
      }
      if (hid == hui::Hit::dialog(hui::Hit::kDlgCompress, hui::Hit::kCompressCancel)) {
        new_hover_btn = 0;
      } else if (hid == hui::Hit::dialog(hui::Hit::kDlgCompress, hui::Hit::kCompressOk)) {
        new_hover_btn = 1;
      }
    }

    bool changed = (new_hover_fmt != app.compress_hover_format) ||
                   (new_hover_lvl != app.compress_hover_level) ||
                   (new_hover_thr != app.compress_hover_threads) ||
                   (new_hover_btn != app.compress_hover_btn);
    if (changed) {
      app.compress_hover_format = new_hover_fmt;
      app.compress_hover_level = new_hover_lvl;
      app.compress_hover_threads = new_hover_thr;
      app.compress_hover_btn = new_hover_btn;
      draw(app);
    }
    return;
  }

  // ── Create dialog button hover ──
  if (app.create_dialog_open) {
    const uint32_t hid = app.hit_main.query(x, y);
    int new_hover_btn = -1;
    if (hid == hui::Hit::dialog(hui::Hit::kDlgCreate, hui::Hit::kCreateOk))
      new_hover_btn = 0;
    else if (hid == hui::Hit::dialog(hui::Hit::kDlgCreate, hui::Hit::kCreateCancel))
      new_hover_btn = 1;
    if (new_hover_btn != app.create_hover_btn) {
      app.create_hover_btn = new_hover_btn;
      draw(app);
      return;
    }
  }

  // ── Rename dialog button hover ──
  if (app.rename_ui_open) {
    const uint32_t hid = app.hit_main.query(x, y);
    int new_hover_btn = -1;
    if (hid == hui::Hit::dialog(hui::Hit::kDlgRename, hui::Hit::kRenameOk))
      new_hover_btn = 0;
    else if (hid == hui::Hit::dialog(hui::Hit::kDlgRename, hui::Hit::kRenameCancel))
      new_hover_btn = 1;
    if (new_hover_btn != app.rename_ui_hover_btn) {
      app.rename_ui_hover_btn = new_hover_btn;
      draw(app);
      return;
    }
  }

  // ── Batch rename dialog hover ──
  if (app.batch_rename_open) {
    const uint32_t hid = app.hit_main.query(x, y);
    using hui::Hit::dialog;
    using hui::Hit::kBatchAdd;
    using hui::Hit::kBatchAddItemBase;
    using hui::Hit::kBatchCancel;
    using hui::Hit::kBatchOk;
    using hui::Hit::kBatchTab0;
    using hui::Hit::kBatchTab1;
    using hui::Hit::kDlgBatch;
    int new_hover_mode = -1;
    if (hid == dialog(kDlgBatch, kBatchTab0))
      new_hover_mode = 0;
    else if (hid == dialog(kDlgBatch, kBatchTab1))
      new_hover_mode = 1;

    int new_hover_btn = -1;
    if (hid == dialog(kDlgBatch, kBatchOk))
      new_hover_btn = 0;
    else if (hid == dialog(kDlgBatch, kBatchCancel))
      new_hover_btn = 1;
    else if (hid == dialog(kDlgBatch, kBatchAdd))
      new_hover_btn = 3;

    int new_add_hover = -1;
    if (app.batch_rename_show_add) {
      int ctrl = hui::Hit::dialog_ctrl(hid);
      int opt = ctrl - kBatchAddItemBase;
      if (opt >= 0 && opt <= 2 &&
          (hid & 0xFFFFC00) == dialog(kDlgBatch, 0))
        new_add_hover = opt;
    }
    if (new_add_hover != app.batch_rename_add_hover) {
      app.batch_rename_add_hover = new_add_hover;
      draw(app);
    }

    bool changed = (new_hover_mode != app.batch_rename_hover_mode) ||
                   (new_hover_btn != app.batch_rename_hover_btn);
    if (changed) {
      app.batch_rename_hover_mode = new_hover_mode;
      app.batch_rename_hover_btn = new_hover_btn;
      draw(app);
    }
    return;
  }

  // ── Properties dialog hover ──
  if (app.properties.open) {
    draw(app);
    return;
  }

  if (app.open_with_open) {
    double dx = static_cast<double>(x), dy = static_cast<double>(y);
    int new_hover = -1;

    if (dx >= app.open_with_hit_close[0] && dx < app.open_with_hit_close[0] + app.open_with_hit_close[2] &&
        dy >= app.open_with_hit_close[1] && dy < app.open_with_hit_close[1] + app.open_with_hit_close[3]) {
      new_hover = -2;
    } else if (dx >= app.open_with_hit_cancel[0] && dx < app.open_with_hit_cancel[0] + app.open_with_hit_cancel[2] &&
               dy >= app.open_with_hit_cancel[1] && dy < app.open_with_hit_cancel[1] + app.open_with_hit_cancel[3]) {
      new_hover = -3;
    } else if (dx >= app.open_with_hit_open[0] && dx < app.open_with_hit_open[0] + app.open_with_hit_open[2] &&
               dy >= app.open_with_hit_open[1] && dy < app.open_with_hit_open[1] + app.open_with_hit_open[3]) {
      new_hover = -4;
    } else if (dx >= app.open_with_hit_default[0] && dx < app.open_with_hit_default[0] + app.open_with_hit_default[2] &&
               dy >= app.open_with_hit_default[1] && dy < app.open_with_hit_default[1] + app.open_with_hit_default[3]) {
      new_hover = -5;
    } else {
      // App list rows resolved through the retained hit registry.
      const uint32_t list_hid = app.hit_main.query(x, y);
      int ctrl = hui::Hit::dialog_ctrl(list_hid);
      int row = ctrl - hui::Hit::kOpenRowBase;
      if (row >= 0 && row < static_cast<int>(app.open_with_apps.size()) &&
          (list_hid & 0xFFFFC00) == hui::Hit::dialog(hui::Hit::kDlgOpenWith, 0)) {
        new_hover = row;
      }
    }

    if (new_hover != app.open_with_hover) {
      app.open_with_hover = new_hover;
      draw(app);
    }
    return;
  }

  if (app.term_chooser_open) {
    const uint32_t hid = app.hit_main.query(x, y);
    int new_hover = -1;
    if (hid == hui::Hit::dialog(hui::Hit::kDlgTerm, hui::Hit::kTermClose)) {
      new_hover = -2;
    } else {
      int ctrl = hui::Hit::dialog_ctrl(hid);
      int idx = ctrl - hui::Hit::kTermRowBase;
      if (idx >= 0 && idx < static_cast<int>(app.term_chooser_apps.size())) new_hover = idx;
    }
    app.term_chooser_hover = new_hover;
    draw(app);
    return;
  }

  // ── Per-pane position helpers ──
  auto& pm_search_btn_x = app.active_pane ? app.r_search_btn_x : app.search_btn_x;
  auto& pm_search_btn_w = app.active_pane ? app.r_search_btn_w : app.search_btn_w;
  auto& pm_folder_search_btn_x = app.active_pane ? app.r_folder_search_btn_x : app.folder_search_btn_x;
  auto& pm_folder_search_btn_w = app.active_pane ? app.r_folder_search_btn_w : app.folder_search_btn_w;
  auto& pm_view_btn_x = app.active_pane ? app.r_view_btn_x : app.view_btn_x;
  auto& pm_view_btn_w = app.active_pane ? app.r_view_btn_w : app.view_btn_w;
  auto& pm_sort_btn_x = app.active_pane ? app.r_sort_btn_x : app.sort_btn_x;
  auto& pm_sort_btn_w = app.active_pane ? app.r_sort_btn_w : app.sort_btn_w;
  auto& pm_dots_btn_x = app.active_pane ? app.r_dots_btn_x : app.dots_btn_x;
  auto& pm_dots_btn_y = app.active_pane ? app.r_dots_btn_y : app.dots_btn_y;
  auto& pm_dots_btn_w = app.active_pane ? app.r_dots_btn_w : app.dots_btn_w;
  auto& pm_dots_btn_h = app.active_pane ? app.r_dots_btn_h : app.dots_btn_h;
  auto& pm_filter_btn_x = app.active_pane ? app.r_filter_btn_x : app.filter_btn_x;
  auto& pm_filter_btn_w = app.active_pane ? app.r_filter_btn_w : app.filter_btn_w;
  auto& pm_filter_dd_x = app.active_pane ? app.r_filter_dropdown_x : app.filter_dropdown_x;
  auto& pm_filter_dd_y = app.active_pane ? app.r_filter_dropdown_y : app.filter_dropdown_y;
  auto& pm_filter_dd_w = app.active_pane ? app.r_filter_dropdown_w : app.filter_dropdown_w;
  auto& pm_filter_dd_h = app.active_pane ? app.r_filter_dropdown_h : app.filter_dropdown_h;
  auto& pm_search_bar_x = app.active_pane ? app.r_search_bar_x : app.search_bar_x;
  auto& pm_search_bar_w = app.active_pane ? app.r_search_bar_w : app.search_bar_w;
  auto& pm_search_clear_x = app.active_pane ? app.r_search_clear_x : app.search_clear_x;
  auto& pm_search_clear_w = app.active_pane ? app.r_search_clear_w : app.search_clear_w;
  auto& pm_breadcrumbs = app.active_pane ? app.r_breadcrumbs : app.breadcrumbs;
  auto& pm_breadcrumb_hover = app.active_pane ? app.r_breadcrumb_hover : app.breadcrumb_hover;

  // ── Top-bar button hover (arrows + view mode + sort + gear + window controls) ──
  {
    int bar_y = y;
    if (app.split_view) {
      int content_y = app.top_bar_height + app.tab_bar_height;
      if (y >= content_y && y < content_y + app.top_bar_height)
        bar_y = y - content_y;
    }
    // Top-bar hovers via retained registry (recalculated every paint).
    const uint32_t thid = app.hit_main.query(x, y);
    auto top_hover = [&](int ctrl) {
      return thid == hui::Hit::topbar(app.active_pane, ctrl);
    };
    // Sidebar fold toggle hover (drawn before the arrows when folded)
    bool toggle_h = (thid == hui::Hit::topbar(0, hui::Hit::kTopFoldToggle) ||
                     thid == hui::Hit::topbar(1, hui::Hit::kTopFoldToggle));
    bool bh = top_hover(hui::Hit::kTopNavBack);
    bool fh = top_hover(hui::Hit::kTopNavForward);
    bool uh = top_hover(hui::Hit::kTopNavUp) && can_navigate_up(app);

    bool vh = (bar_y < app.top_bar_height && x >= pm_view_btn_x && x < pm_view_btn_x + pm_view_btn_w);
    bool search_h = (bar_y < app.top_bar_height && x >= pm_search_btn_x && x < pm_search_btn_x + pm_search_btn_w);
    bool folder_search_h = (bar_y < app.top_bar_height && x >= pm_folder_search_btn_x && x < pm_folder_search_btn_x + pm_folder_search_btn_w);
    bool sh = (bar_y < app.top_bar_height && x >= pm_sort_btn_x && x < pm_sort_btn_x + pm_sort_btn_w);
    bool filter_h = ((app.active_pane ? app.r_search_active : app.search_active) || (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active)) &&
                     bar_y < app.top_bar_height && x >= pm_filter_btn_x && x < pm_filter_btn_x + pm_filter_btn_w;

    bool gh = (thid == hui::Hit::topbar(app.active_pane, hui::Hit::kTopGear));

    bool dh = (pm_dots_btn_w > 0 &&
               bar_y >= pm_dots_btn_y && bar_y < pm_dots_btn_y + pm_dots_btn_h &&
               x >= pm_dots_btn_x && x < pm_dots_btn_x + pm_dots_btn_w);

    // Window control buttons (traffic lights on right: max | min | close)
    bool close_h = false, min_h = false, max_h = false;
    if (app.win_btn_x > 0 && bar_y < app.top_bar_height) {
      int rel = x - app.win_btn_x;
      if (rel >= 0 && rel < 3 * app.win_btn_w) {
        int slot = rel / app.win_btn_w;
        max_h   = (slot == 0);
        min_h   = (slot == 1);
        close_h = (slot == 2);
      }
    }

    // Filter-bar control hovers (mode segment / case / lock)
    bool fb_active = (app.active_pane ? app.r_search_active : app.search_active) ||
                     (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active);
    bool fmode_h = false, fcase_h = false, flock_h = false;
    int fmode_btn = -1;
    if (fb_active) {
      auto fm_x = app.active_pane ? app.r_search_mode_x : app.search_mode_x;
      auto fm_w = app.active_pane ? app.r_search_mode_w : app.search_mode_w;
      auto fc_x = app.active_pane ? app.r_search_case_x : app.search_case_x;
      auto fc_w = app.active_pane ? app.r_search_case_w : app.search_case_w;
      auto fl_x = app.active_pane ? app.r_search_lock_x : app.search_lock_x;
      auto fl_w = app.active_pane ? app.r_search_lock_w : app.search_lock_w;
      if (fm_w > 0 && x >= fm_x && x < fm_x + fm_w) {
        fmode_h = true;
        fmode_btn = (x - fm_x) / (fm_w / 4);
      }
      fcase_h = fc_w > 0 && x >= fc_x && x < fc_x + fc_w;
      flock_h = fl_w > 0 && x >= fl_x && x < fl_x + fl_w;
    }

    if (bh != (app.active_pane ? app.r_arrow_back_hover : app.arrow_back_hover) ||
        fh != (app.active_pane ? app.r_arrow_forward_hover : app.arrow_forward_hover) ||
        uh != (app.active_pane ? app.r_arrow_up_hover : app.arrow_up_hover) ||
        toggle_h != app.sidebar_toggle_hover ||
        vh != (app.active_pane ? app.r_view_mode_btn_hover : app.view_mode_btn_hover) ||
        search_h != (app.active_pane ? app.r_search_btn_hover : app.search_btn_hover) ||
        folder_search_h != (app.active_pane ? app.r_folder_search_btn_hover : app.folder_search_btn_hover) ||
        sh != (app.active_pane ? app.r_sort_btn_hover : app.sort_btn_hover) ||
        gh != (app.active_pane ? app.r_settings_btn_hover : app.settings_btn_hover) ||
        dh != (app.active_pane ? app.r_dots_btn_hover : app.dots_btn_hover) ||
        filter_h != (app.active_pane ? app.r_filter_btn_hover : app.filter_btn_hover) ||
        fmode_h != (app.active_pane ? app.r_search_mode_hover : app.search_mode_hover) ||
        fcase_h != (app.active_pane ? app.r_search_case_hover : app.search_case_hover) ||
        flock_h != (app.active_pane ? app.r_search_lock_hover : app.search_lock_hover) ||
        fmode_btn != (app.active_pane ? app.r_search_mode_hover_btn : app.search_mode_hover_btn) ||
        close_h != app.win_btn_close_hover || min_h != app.win_btn_min_hover ||
        max_h != app.win_btn_max_hover) {
      (app.active_pane ? app.r_arrow_back_hover : app.arrow_back_hover) = bh;
      (app.active_pane ? app.r_arrow_forward_hover : app.arrow_forward_hover) = fh;
      (app.active_pane ? app.r_arrow_up_hover : app.arrow_up_hover) = uh;
      app.sidebar_toggle_hover = toggle_h;
      (app.active_pane ? app.r_view_mode_btn_hover : app.view_mode_btn_hover) = vh;
      (app.active_pane ? app.r_search_btn_hover : app.search_btn_hover) = search_h;
      (app.active_pane ? app.r_folder_search_btn_hover : app.folder_search_btn_hover) = folder_search_h;
      (app.active_pane ? app.r_sort_btn_hover : app.sort_btn_hover) = sh;
      (app.active_pane ? app.r_settings_btn_hover : app.settings_btn_hover) = gh;
      (app.active_pane ? app.r_dots_btn_hover : app.dots_btn_hover) = dh;
      (app.active_pane ? app.r_filter_btn_hover : app.filter_btn_hover) = filter_h;
      (app.active_pane ? app.r_search_mode_hover : app.search_mode_hover) = fmode_h;
      (app.active_pane ? app.r_search_case_hover : app.search_case_hover) = fcase_h;
      (app.active_pane ? app.r_search_lock_hover : app.search_lock_hover) = flock_h;
      (app.active_pane ? app.r_search_mode_hover_btn : app.search_mode_hover_btn) = fmode_btn;
      app.win_btn_close_hover = close_h;
      app.win_btn_min_hover = min_h;
      app.win_btn_max_hover = max_h;
      draw(app);
      return;
    }
  }

  // ── Directory picker bar hover ──
  if (app.select_dir_mode || app.select_file_mode) {
    bool sh = (y >= app.select_bar_y && y < app.select_bar_y + app.select_bar_h &&
               x >= app.select_btn_x && x < app.select_btn_x + app.select_btn_w);
    bool ch = (y >= app.select_bar_y && y < app.select_bar_y + app.select_bar_h &&
               x >= app.cancel_btn_x && x < app.cancel_btn_x + app.cancel_btn_w);
    if (sh != app.select_btn_hover || ch != app.cancel_btn_hover) {
      app.select_btn_hover = sh;
      app.cancel_btn_hover = ch;
      draw(app);
      return;
    }
  }

  // ── Sort menu item hover ──
  if ((app.active_pane ? app.r_sort_menu_open : app.sort_menu_open)) {
    const uint32_t hid = app.hit_main.query(x, y);
    int new_hover = -1;
    if ((hid & hui::Hit::kGroupMask) == hui::Hit::kMenu &&
        hui::Hit::menu_id(hid) == hui::Hit::kMenuSort) {
      int idx = hui::Hit::menu_row(hid);
      if (idx >= 0 && idx < sort_menu_row_count() &&
          sort_menu_row(idx).kind != SortMenuRow::Kind::Separator &&
          sort_menu_row(idx).kind != SortMenuRow::Kind::GroupCaption)
        new_hover = idx;
    }
    if (new_hover != (app.active_pane ? app.r_sort_menu_hover : app.sort_menu_hover)) {
      (app.active_pane ? app.r_sort_menu_hover : app.sort_menu_hover) = new_hover;
      draw(app);
      return;
    }
  }

  // ── Column chooser item hover ──
  if ((app.active_pane ? app.r_columns_menu_open : app.columns_menu_open)) {
    auto& cmh_hover = app.active_pane ? app.r_columns_menu_hover : app.columns_menu_hover;
    const uint32_t hid = app.hit_main.query(x, y);
    int new_hover = -1;
    if ((hid & hui::Hit::kGroupMask) == hui::Hit::kMenu &&
        hui::Hit::menu_id(hid) == hui::Hit::kMenuColumns) {
      int idx = hui::Hit::menu_row(hid);
      if (idx >= 0 && idx <= 4) new_hover = idx;
    }
    if (new_hover != cmh_hover) {
      cmh_hover = new_hover;
      draw(app);
      return;
    }
  }

  // ── Filter dropdown item hover ──
  {
    auto& pm_filter_section = app.active_pane ? app.r_filter_dropdown_section : app.filter_dropdown_section;
    auto& pm_filter_hover = app.active_pane ? app.r_filter_dropdown_hover : app.filter_dropdown_hover;
    if (pm_filter_section > 0) {
      const uint32_t hid = app.hit_main.query(x, y);
      int new_hover = -1;
      if ((hid & hui::Hit::kGroupMask) == hui::Hit::kMenu &&
          hui::Hit::menu_id(hid) == hui::Hit::kMenuFilter) {
        int ctrl = hui::Hit::menu_row(hid);
        if (ctrl >= 200) {
          // Header: glob is the header's position in the walk (counts only).
          int si = ctrl - 200;
          int glob = 0;
          for (int s = 1; s < si; ++s)
            glob += 1 + (pm_filter_section == s ? (s == 1 ? 13 : s == 2 ? 7 : 5) : 0);
          new_hover = glob;
        } else {
          new_hover = ctrl;
        }
      }
      if (new_hover != pm_filter_hover) {
        pm_filter_hover = new_hover;
        draw(app);
        return;
      }
    }
  }

  // ── Path editing drag selection ──
  if ((app.active_pane ? app.r_path_editing : app.path_editing) && (app.active_pane ? app.r_path_edit_dragging : app.path_edit_dragging)) {
    int pm_pe_bar_y = y;
    if (app.split_view) {
      int content_y = app.top_bar_height + app.tab_bar_height;
      if (y >= content_y && y < content_y + app.top_bar_height)
        pm_pe_bar_y = y - content_y;
    }
    if (pm_pe_bar_y < app.top_bar_height) {
    auto& pm_pe_buf = app.active_pane ? app.r_path_edit_buf : app.path_edit_buf;
    auto& pm_pe_cursor = app.active_pane ? app.r_path_edit_cursor : app.path_edit_cursor;
    auto& pm_pe_sel_start = app.active_pane ? app.r_path_edit_sel_start : app.path_edit_sel_start;
    auto& pm_pe_sel_end = app.active_pane ? app.r_path_edit_sel_end : app.path_edit_sel_end;
    double zf = app.zoom_pct / 100.0;
    const hui::HitRegion* pe_field =
        app.hit_main.find_id(hui::Hit::topbar(app.active_pane, hui::Hit::kTopPathText));
    if (pe_field != nullptr) {
    int path_w_inner = pe_field->w;
    int text_x = pe_field->x;
    cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr_tmp = cairo_create(tmp);
    cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr_tmp, 13.0 * zf);
    const std::string& buf = pm_pe_buf;
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
    if (best_pos != pm_pe_cursor || pm_pe_sel_start < 0) {
      if (pm_pe_sel_start < 0) {
        pm_pe_sel_start = pm_pe_cursor;
        // Force at least 1-char highlight on the very first drag move
        // so selection appears immediately rather than waiting for a
        // character-boundary crossing.
        if (best_pos == pm_pe_cursor &&
            pm_pe_cursor < static_cast<int>(pm_pe_buf.size())) {
          pm_pe_cursor = pm_pe_cursor + 1;
        } else {
          pm_pe_cursor = best_pos;
        }
      } else {
        pm_pe_cursor = best_pos;
      }
      pm_pe_sel_end = pm_pe_cursor;
      draw(app);
    }
    } // pe_field
    return;
    }
  }

  // ── Dialog input field drag selection ──
  {
    auto helper_drag = [&](const std::string& buf, int& cursor, int& sel_start, int& sel_end,
                           int input_x, int input_y, int input_w, int input_h, double font_size) {
      if (x < input_x || x >= input_x + input_w || y < input_y || y >= input_y + input_h)
        return;
      cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t* cr_tmp = cairo_create(tmp);
      cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr_tmp, font_size);
      int click_x = x - input_x - static_cast<int>(font_size);
      int best_pos = static_cast<int>(buf.size());
      for (int ci = 0; ci <= static_cast<int>(buf.size()); ++ci) {
        cairo_text_extents_t te;
        cairo_text_extents(cr_tmp, buf.substr(0, static_cast<std::size_t>(ci)).c_str(), &te);
        if (te.width >= click_x) { best_pos = ci; break; }
      }
      cairo_destroy(cr_tmp);
      cairo_surface_destroy(tmp);
      if (best_pos != cursor || sel_start < 0) {
        if (sel_start < 0) {
          sel_start = cursor;
          if (best_pos == cursor && cursor < static_cast<int>(buf.size()))
            cursor = cursor + 1;
          else
            cursor = best_pos;
        } else {
          cursor = best_pos;
        }
        sel_end = cursor;
      }
    };

    if (app.create_dialog_open && app.create_dragging) {
      int dlg_w = 340, dlg_h = 160;
      int dlg_x = (app.width - dlg_w) / 2, dlg_y = (app.height - dlg_h) / 2;
      int input_x = dlg_x + 20, input_y = dlg_y + 50, input_w = dlg_w - 40, input_h = 34;
      helper_drag(app.create_buf, app.create_cursor_pos,
                  app.create_sel_start, app.create_sel_end,
                  input_x, input_y, input_w, input_h, 14.0);
      draw(app);
      return;
    }
    if (app.rename_ui_open && app.rename_ui_dragging) {
      int dlg_w = 400, dlg_h = 190;
      int dlg_x = (app.width - dlg_w) / 2, dlg_y = (app.height - dlg_h) / 2;
      int input_x = dlg_x + 24, input_y = dlg_y + 64, input_w = dlg_w - 48, input_h = 36;
      helper_drag(app.rename_ui_buf, app.rename_ui_cursor_pos,
                  app.rename_ui_sel_start, app.rename_ui_sel_end,
                  input_x, input_y, input_w, input_h, 14.0);
      draw(app);
      return;
    }
    if (app.password_dialog_open && app.password_dragging) {
      int card_w = 400, card_h = 210, pad = 24;
      int cx = (app.width - card_w) / 2, cy = (app.height - card_h) / 2;
      int input_x = cx + pad, input_y = cy + pad + 62, input_w = card_w - pad * 2, input_h = 36;
      std::string masked(app.password_buf.size(), '*');
      helper_drag(masked, app.password_cursor_pos,
                  app.password_sel_start, app.password_sel_end,
                  input_x, input_y, input_w, input_h, 14.0);
      draw(app);
      return;
    }
  }

  // ── Breadcrumb hover tracking ──
  {
    int pm_bc_bar_y = y;
    if (app.split_view) {
      int content_y = app.top_bar_height + app.tab_bar_height;
      if (y >= content_y && y < content_y + app.top_bar_height)
        pm_bc_bar_y = y - content_y;
    }
    if (pm_bc_bar_y < app.top_bar_height && !(app.active_pane ? app.r_path_editing : app.path_editing)) {
      int hover = -1;
      for (size_t i = 0; i < pm_breadcrumbs.size(); ++i) {
        if (x >= pm_breadcrumbs[i].x && x < pm_breadcrumbs[i].x + pm_breadcrumbs[i].w) {
          hover = static_cast<int>(i);
          break;
        }
      }
      if (hover != pm_breadcrumb_hover) {
        pm_breadcrumb_hover = hover;
        draw(app);
        return;
      }
    } else if (pm_breadcrumb_hover >= 0) {
      pm_breadcrumb_hover = -1;
      draw(app);
      return;
    }
  }

  // ── Column divider hover ──
  if (!(app.active_pane ? app.r_path_editing : app.path_editing) && app.cur_tab().view_mode == ViewMode::List && y >= app.top_bar_height + app.tab_bar_height &&
      y < app.top_bar_height + app.tab_bar_height + app.entry_height) {
    int sidebar_w = app.sidebar_w();
    double zf = app.zoom_pct / 100.0;
    int icon_size = static_cast<int>(24.0 * zf);
    int text_x = sidebar_w + icon_size + static_cast<int>(12.0 * zf);
    int content_w = app.width - sidebar_w;
    int name_w = static_cast<int>(content_w * app.col_name_frac);
    int size_w = static_cast<int>(content_w * app.col_size_frac);
    int date_w = static_cast<int>(content_w * app.col_date_frac);
    int col_y = app.top_bar_height + app.tab_bar_height;
    int col_h = app.entry_height;
    int x1 = text_x + name_w;
    int x2 = x1 + size_w;
    int x3 = x2 + date_w;
    bool new_col_hover = (abs(x - x1) < 4 || abs(x - x2) < 4 || abs(x - x3) < 4) &&
                          y >= col_y && y < col_y + col_h;
    if (new_col_hover != app.col_hover_divider) {
      app.col_hover_divider = new_col_hover;
      draw(app);
      return;
    }
  } else if (app.col_hover_divider) {
    app.col_hover_divider = false;
    draw(app);
    return;
  }

  // ── Column divider drag ──
  if (app.col_resizing >= 0 && app.cur_tab().view_mode == ViewMode::List) {
    int sidebar_w = app.sidebar_w();
    int content_w = app.width - sidebar_w;
    double frac = static_cast<double>(x - sidebar_w) / static_cast<double>(std::max(1, content_w));
    frac = std::clamp(frac, 0.05, 0.85);
    double delta = frac - app.col_resize_start_frac;
    if (app.col_resizing == 0) {
      app.col_name_frac = std::clamp(app.col_resize_start_frac + delta, 0.10, 0.80);
    } else if (app.col_resizing == 1) {
      app.col_size_frac = std::clamp(app.col_resize_start_frac + delta, 0.05, 0.50);
    } else if (app.col_resizing == 2) {
      app.col_date_frac = std::clamp(app.col_resize_start_frac + delta, 0.05, 0.50);
    }
    draw(app);
    return;
  }

  int sb_idx = hit_test_sidebar(app, x, y);
  app.sidebar_hover_idx = sb_idx;

  // Mount indicator hover — check if pointer is over the indicator zone of a mounted drive
  app.sidebar_mount_hover_idx = -1;
  if (sb_idx >= 0 && sb_idx < static_cast<int>(app.sidebar_locations.size())) {
    auto& loc = app.sidebar_locations[sb_idx];
    if (loc.kind == SidebarLocation::Kind::Drive && loc.is_mounted) {
      double zf = 1.2;
      int ind_sz = static_cast<int>(18.0 * zf);
      int ind_x = app.effective_sidebar_width() - static_cast<int>(24.0 * zf);
      if (x >= ind_x - 4 && x <= ind_x + ind_sz + 4)
        app.sidebar_mount_hover_idx = sb_idx;
    }
  }

  // Store previous hover before updating
  int prev_hover = app.cur_tab().hover_idx;

  if (!app.context_menu_open) {
    int new_hover;
    if (app.cur_tab().view_mode == ViewMode::List) {
      new_hover = hit_test_list(app, x, y);
    } else if (app.cur_tab().view_mode == ViewMode::Grid) {
      new_hover = hit_test_grid(app, x, y);
    } else if (app.cur_tab().view_mode == ViewMode::Tree) {
      new_hover = hit_test_tree(app, x, y);
      if (new_hover == -2) {
        // Arrow hover: recalc actual entry index from y-position
        double zf = app.zoom_pct / 100.0;
        int entry_h = static_cast<int>(28.0 * zf);
        int content_y = app.top_bar_height + app.tab_bar_height;
        if (app.split_view) content_y += app.top_bar_height;
        int rel_y = y - content_y + app.cur_tab().scroll_px;
        int ti = rel_y / entry_h;
        if (ti >= 0 && ti < static_cast<int>(app.cur_tab().tree_entries.size()))
          new_hover = ti;
        else
          new_hover = -1;
      }
    } else if (app.cur_tab().view_mode == ViewMode::Compact) {
      new_hover = hit_test_compact(app, x, y);
    } else {
      new_hover = -1;
    }

    // Lock hover to preview entry while hover preview is active
    if (app.preview_mode == AppState::PreviewMode::Hover && app.preview_entry_idx >= 0) {
      if (new_hover < 0) {
        // Mouse left the entries area — dismiss preview
        reset_preview(app);
        app.cur_tab().hover_idx = -1;
      } else {
        app.cur_tab().hover_idx = app.preview_entry_idx;
      }
    } else {
      app.cur_tab().hover_idx = new_hover;
    }

    if (app.cur_tab().view_mode == ViewMode::Computer) {
      app.computer_hover_idx = hit_test_computer(app, x, y);
    }
  } else {
    app.cur_tab().hover_idx = -1;
    app.computer_hover_idx = -1;
  }

  // ── Hover preview timer ──
  if (app.cur_tab().hover_idx != prev_hover &&
      app.preview_mode != AppState::PreviewMode::Space) {
    reset_preview(app);
    hide_tooltip(app);
    if (app.cur_tab().hover_idx >= 0) {
      int vi = app.cur_tab().hover_idx;
      if (vi >= 0 && vi < static_cast<int>(app.cur_tab().visible_entries.size())) {
        int ri = app.cur_tab().visible_entries[vi];
        if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size())) {
          const auto& entry = app.cur_tab().entries[ri];
          if (is_supported_preview(entry)) {
            app.preview_entry_idx = app.cur_tab().hover_idx;
            app.preview_path = entry.path;
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            app.preview_hover_start_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                                          static_cast<uint64_t>(ts.tv_nsec);
          } else if (entry.is_dir) {
            // Rich tooltip for entries without a live preview (folders)
            app.tooltip_path = entry.path;
            app.tooltip_active = false;
            app.tooltip_show_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() + 200;
          }
        }
      }
    }
  }

  if (app.drop_chooser_open) {
    int hover = hit_test_drop_chooser(app, x, y);
    if (hover != app.drop_chooser_hover) app.drop_chooser_hover = hover;
  }

  if (app.context_menu_open) {
    int prev = app.context_menu_hover;
    int hit = hit_test_context_menu(app, x, y);
    app.context_menu_sub_hover = -1;
    if (hit < -9) {
      // Submenu item hovered: parent = context_menu_hover_prev, sub = -(hit + 10)
      app.context_menu_sub_hover = -(hit + 10);
      int parent = app.context_menu_hover_prev;
      if (parent >= 0 && static_cast<size_t>(parent) < app.context_menu_items.size())
        app.context_menu_hover = parent;
      else
        app.context_menu_hover = -1;
    } else {
      app.context_menu_hover = hit;
      // Keep submenu open when hovering its area (between items / separator)
      if (app.context_menu_hover < 0 && prev >= 0 &&
          static_cast<size_t>(prev) < app.context_menu_items.size() &&
          !app.context_menu_items[prev].sub_items.empty()) {
        app.context_menu_hover = prev;
      }
    }
    if (app.context_menu_hover >= 0 &&
        static_cast<size_t>(app.context_menu_hover) < app.context_menu_items.size() &&
        !app.context_menu_items[app.context_menu_hover].sub_items.empty()) {
      app.context_menu_hover_prev = app.context_menu_hover;
    }
  }
  draw(app);
}


} // namespace eh::file_browser
