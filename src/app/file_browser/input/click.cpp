// click.cpp — click hit-testing and the click handler.
// handle_click, properties_hit_test / settings_hit_test and the modal
// gate moved wholesale from events.cpp (byte-identical).
#include "events.hpp"
#include "../app.hpp"
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
// ── properties + settings hit tests, click handler (moved from events.cpp) ──────────────────────────
int properties_hit_test(AppState& app, int x, int y) {
  const auto& p = app.properties;
  if (!p.open) return -1;
  const int card_w = static_cast<int>(p.w);
  const int card_h = static_cast<int>(p.h);
  const int cx = static_cast<int>(p.x);
  const int cy = static_cast<int>(p.y);

  if (x < cx || x >= cx + card_w || y < cy || y >= cy + card_h)
    return -1;

  // Close X button
  if (x >= p.hit_close[0] && x < p.hit_close[0] + p.hit_close[2] &&
      y >= p.hit_close[1] && y < p.hit_close[1] + p.hit_close[3])
    return -2;

  // Bottom "Close" button
  if (x >= p.hit_close_btn[0] && x < p.hit_close_btn[0] + p.hit_close_btn[2] &&
      y >= p.hit_close_btn[1] && y < p.hit_close_btn[1] + p.hit_close_btn[3])
    return -2;

  // Tab clicks (only if no combo open)
  if (p.combo_open < 0) {
    int num_tabs = 2;
    if (p.image_w > 0 && p.image_h > 0) ++num_tabs;
    if (p.is_media) ++num_tabs;
    for (int t = 0; t < num_tabs; ++t) {
      if (x >= p.hit_tabs[t][0] && x < p.hit_tabs[t][0] + p.hit_tabs[t][2] &&
          y >= p.hit_tabs[t][1] && y < p.hit_tabs[t][1] + p.hit_tabs[t][3])
        return -10 - t; // -10=Basic, -11=Permissions, -12=Image, -13=Media
    }
  }

  // If a combo is open, check dropdown items first (they take priority)
  if (p.combo_open >= 0 && p.combo_open < 3) {
    int pi = p.combo_open;
    for (int ci = 0; ci < 4; ++ci) {
      if (x >= p.hit_combo_items[pi][ci][0] && x < p.hit_combo_items[pi][ci][0] + p.hit_combo_items[pi][ci][2] &&
          y >= p.hit_combo_items[pi][ci][1] && y < p.hit_combo_items[pi][ci][1] + p.hit_combo_items[pi][ci][3])
        return 200 + pi * 4 + ci;
    }
  }

  // Combo boxes
  for (int pi = 0; pi < 3; ++pi) {
    if (x >= p.hit_combo[pi][0] && x < p.hit_combo[pi][0] + p.hit_combo[pi][2] &&
        y >= p.hit_combo[pi][1] && y < p.hit_combo[pi][1] + p.hit_combo[pi][3])
      return 10 + pi; // 10=owner, 11=group, 12=other
  }

  // Executable toggle
  if (!p.is_dir && p.hit_exec_toggle[2] > 0) {
    if (x >= p.hit_exec_toggle[0] && x < p.hit_exec_toggle[0] + p.hit_exec_toggle[2] &&
        y >= p.hit_exec_toggle[1] && y < p.hit_exec_toggle[1] + p.hit_exec_toggle[3])
      return 15;
  }

  // Numeric octal mode editor row (Permissions tab, single selection)
  if (!p.multi && p.hit_octal[2] > 0) {
    if (x >= p.hit_octal[0] && x < p.hit_octal[0] + p.hit_octal[2] &&
        y >= p.hit_octal[1] && y < p.hit_octal[1] + p.hit_octal[3])
      return 16;
  }

  // Tags row (Basic tab, single selection)
  if (!p.multi && p.hit_tags_row[2] > 0) {
    if (x >= p.hit_tags_row[0] && x < p.hit_tags_row[0] + p.hit_tags_row[2] &&
        y >= p.hit_tags_row[1] && y < p.hit_tags_row[1] + p.hit_tags_row[3])
      return 17;
  }

  return 0; // inside dialog but no specific widget
}

// ── settings hit test ────────────────────────────────────────────

int settings_hit_test(AppState& app, int x, int y) {
  const int card_w = settings_dialog_width();
  const int card_h = settings_dialog_card_height(app);
  const int cx = (app.width - card_w) / 2;
  const int cy = (app.height - card_h) / 2;
  const int pad = 20;

  if (x < cx || x >= cx + card_w || y < cy || y >= cy + card_h)
    return -1;

  {
    const int close_x = cx + card_w - pad - 24;
    const int close_y = cy + 8;
    if (x >= close_x && x < close_x + 24 && y >= close_y && y < close_y + 24)
      return -2;
  }

  const int top_bar_h = 44;
  const int tab_y = cy + top_bar_h + 4;
  const int tab_w = (card_w - 2 * pad) / 3;
  const int tab_h = 36;

  {
    const int tx = cx + pad;
    if (x >= tx && x < tx + tab_w && y >= tab_y && y < tab_y + tab_h)
      return -3;
  }
  {
    const int tx = cx + pad + tab_w;
    if (x >= tx && x < tx + tab_w && y >= tab_y && y < tab_y + tab_h)
      return -4;
  }
  {
    const int tx = cx + pad + tab_w * 2;
    if (x >= tx && x < tx + tab_w && y >= tab_y && y < tab_y + tab_h)
      return -22;
  }

  const int content_y = tab_y + tab_h + 12;
  const int btn_y = cy + card_h - 50;
  const int btn_h = 30;
  const int btn_w = 80;
  const int btn_gap = 10;

  {
    const int ok_x = cx + card_w - pad - btn_w * 3 - btn_gap * 2;
    if (x >= ok_x && x < ok_x + btn_w && y >= btn_y && y < btn_y + btn_h)
      return -5;
  }
  {
    const int apply_x = cx + card_w - pad - btn_w;
    if (x >= apply_x && x < apply_x + btn_w && y >= btn_y && y < btn_y + btn_h)
      return -6;
  }
  {
    const int cancel_x = cx + card_w - pad - btn_w * 2 - btn_gap;
    if (x >= cancel_x && x < cancel_x + btn_w && y >= btn_y && y < btn_y + btn_h)
      return -7;
  }

  const int left_x = cx + 28;
  const int ly = content_y;
  const int z_btn_y = ly - 4;
  const int z_btn_s = 28;

  {
    const int zoom_minus_x = left_x + 220;
    if (x >= zoom_minus_x && x < zoom_minus_x + z_btn_s &&
        y >= z_btn_y && y < z_btn_y + z_btn_s)
      return -8;
  }
  {
    const int zoom_plus_x = left_x + 220 + z_btn_s + 6;
    if (x >= zoom_plus_x && x < zoom_plus_x + z_btn_s &&
        y >= z_btn_y && y < z_btn_y + z_btn_s)
      return -9;
  }

  // Click on zoom value text → inline edit
  {
    const int zoom_val_x = left_x + 180;
    const int zoom_val_y = ly - 2;
    const int zoom_val_w = 36;
    const int zoom_val_h = 22;
    if (x >= zoom_val_x && x < zoom_val_x + zoom_val_w &&
        y >= zoom_val_y && y < zoom_val_y + zoom_val_h) {
      if (!app.settings_zoom_editing) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", app.settings_zoom_pct);
        app.settings_zoom_buf = buf;
        app.settings_zoom_editing = true;
      }
      return -16;
    }
  }

  {
    const int toggle_x = left_x + 220;
    const int toggle_y = content_y + 40 - 2;
    const int toggle_w = 40;
    const int toggle_h = 22;
    if (x >= toggle_x && x < toggle_x + toggle_w &&
        y >= toggle_y && y < toggle_y + toggle_h)
      return -10;
  }

  {
    const int slider_x = cx + pad + 8;
    const int slider_y = content_y + 24;
    const int slider_w = card_w - 2 * pad - 16;
    const int slider_h = 6;
    if (x >= slider_x && x < slider_x + slider_w &&
        y >= slider_y - 10 && y < slider_y + slider_h + 20)
      return -11;
  }

  // Sidebar opacity slider (Appearance tab, below surface opacity slider)
  if (app.settings_tab == 1) {
    const int sb_slider_y = content_y + 76;
    const int sb_slider_w = card_w - 2 * pad - 16;
    const int sb_slider_h = 6;
    if (x >= cx + pad + 8 && x < cx + pad + 8 + sb_slider_w &&
        y >= sb_slider_y - 10 && y < sb_slider_y + sb_slider_h + 20)
      return -13;
  }

  // Top bar opacity slider (Appearance tab)
  if (app.settings_tab == 1) {
    const int tb_slider_y = content_y + 128;
    const int tb_slider_w = card_w - 2 * pad - 16;
    const int tb_slider_h = 6;
    if (x >= cx + pad + 8 && x < cx + pad + 8 + tb_slider_w &&
        y >= tb_slider_y - 10 && y < tb_slider_y + tb_slider_h + 20)
      return -14;
  }

  // Status bar opacity slider (Appearance tab)
  if (app.settings_tab == 1) {
    const int st_slider_y = content_y + 180;
    const int st_slider_w = card_w - 2 * pad - 16;
    const int st_slider_h = 6;
    if (x >= cx + pad + 8 && x < cx + pad + 8 + st_slider_w &&
        y >= st_slider_y - 10 && y < st_slider_y + st_slider_h + 20)
      return -15;
  }

  // Preview opacity slider (Appearance tab)
  if (app.settings_tab == 1) {
    const int pv_slider_y = content_y + 232;
    const int pv_slider_w = card_w - 2 * pad - 16;
    const int pv_slider_h = 6;
    if (x >= cx + pad + 8 && x < cx + pad + 8 + pv_slider_w &&
        y >= pv_slider_y - 10 && y < pv_slider_y + pv_slider_h + 20)
      return -17;
  }

  // Settings dialog opacity slider (Appearance tab)
  if (app.settings_tab == 1) {
    const int dlg_slider_y = content_y + 284;
    const int dlg_slider_w = card_w - 2 * pad - 16;
    const int dlg_slider_h = 6;
    if (x >= cx + pad + 8 && x < cx + pad + 8 + dlg_slider_w &&
        y >= dlg_slider_y - 10 && y < dlg_slider_y + dlg_slider_h + 20)
      return -19;
  }

  // Properties dialog opacity slider (Appearance tab)
  if (app.settings_tab == 1) {
    const int prp_slider_y = content_y + 336;
    const int prp_slider_w = card_w - 2 * pad - 16;
    const int prp_slider_h = 6;
    if (x >= cx + pad + 8 && x < cx + pad + 8 + prp_slider_w &&
        y >= prp_slider_y - 10 && y < prp_slider_y + prp_slider_h + 20)
      return -20;
  }

  // Preview scale slider (Preview tab)
  if (app.settings_tab == 2) {
    const int sc_slider_y = content_y + 24;
    const int sc_slider_w = card_w - 2 * pad - 16;
    const int sc_slider_h = 6;
    if (x >= cx + pad + 8 && x < cx + pad + 8 + sc_slider_w &&
        y >= sc_slider_y - 10 && y < sc_slider_y + sc_slider_h + 20)
      return -23;
  }

  // Matugen theming toggle (Appearance tab)
  if (app.settings_tab == 1) {
    const int toggle_x = static_cast<int>(app.settings_hit_matugen_toggle[0]);
    const int toggle_y = static_cast<int>(app.settings_hit_matugen_toggle[1]);
    const int toggle_w = static_cast<int>(app.settings_hit_matugen_toggle[2]);
    const int toggle_h = static_cast<int>(app.settings_hit_matugen_toggle[3]);
    if (toggle_w > 0 && x >= toggle_x && x < toggle_x + toggle_w &&
        y >= toggle_y && y < toggle_y + toggle_h)
      return -18;
  }

  // Color engine sync toggle (Appearance tab)
  if (app.settings_tab == 1) {
    const int toggle_x = static_cast<int>(app.settings_hit_color_engine_toggle[0]);
    const int toggle_y = static_cast<int>(app.settings_hit_color_engine_toggle[1]);
    const int toggle_w = static_cast<int>(app.settings_hit_color_engine_toggle[2]);
    const int toggle_h = static_cast<int>(app.settings_hit_color_engine_toggle[3]);
    if (toggle_w > 0 && x >= toggle_x && x < toggle_x + toggle_w &&
        y >= toggle_y && y < toggle_y + toggle_h)
      return -24;
  }

  {
    const int drop_x = left_x + 130;
    const int drop_y = content_y + 76;
    const int drop_w = 226;
    const int drop_h = 30;

    if (x >= drop_x && x < drop_x + drop_w &&
        y >= drop_y && y < drop_y + drop_h)
      return -12;

    if (app.settings_dropdown_open) {
      const int dd_y = drop_y + drop_h + 2;
      const int dd_entry_h = 28;
      const int total = static_cast<int>(app.settings_term_opts.size());
      int remaining = total - app.settings_dropdown_scroll;
      int visible = std::min(remaining, 6);

      for (int i = 0; i < visible; ++i) {
        int item_y = dd_y + i * dd_entry_h;
        if (y >= item_y && y < item_y + dd_entry_h)
          return i;
      }
    }
  }

  // Independent views per directory toggle (General tab)
  if (!app.settings_dropdown_open) {
    const int toggle_x = left_x + 220;
    const int toggle_y = content_y + 120 - 2;
    const int toggle_w = 40;
    const int toggle_h = 22;
    if (x >= toggle_x && x < toggle_x + toggle_w &&
        y >= toggle_y && y < toggle_y + toggle_h)
      return -21;
  }

  return -1;
}

// ── event handling ───────────────────────────────────────────────

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

void handle_click(AppState& app, int x, int y, int button) {
  hide_tooltip(app);
  app.pointerX = static_cast<double>(x);
  app.pointerY = static_cast<double>(y);

  // ── Adaptive sidebar flap: clicks outside dismiss it ──
  // The flap is a transient overlay, so clicking anywhere outside the flap
  // closes it. The click is still processed normally (it acts on whatever
  // was clicked), matching the Nautilus AdwFlap behavior.
  dismiss_sidebar_flap(app, x, y);

  // ── Drop action chooser (Copy/Move prompt) ──
  // A click on a row picks that action; any other click (either button)
  // dismisses the prompt without acting.
  if (app.drop_chooser_open) {
    int dc = hit_test_drop_chooser(app, x, y);
    if (dc >= 0 && button == 0x110) {
      resolve_drop_chooser(app, dc);
    } else if (button == 0x110 || button == 0x111) {
      app.drop_chooser_open = false;
      app.drop_chooser_hover = -1;
      app.drop_chooser_srcs.clear();
      app.drop_chooser_target.clear();
    }
    draw(app);
    return;
  }

  // ── Context menu clicks ──
  // Dispatched before the region handlers below: the menu is drawn above
  // the status bar / scrollbar, so its rows must win over those handlers,
  // which otherwise swallow any click in their band (e.g. the status-bar
  // zoom guard at the bottom edge returns unconditionally, making menu
  // rows near the bottom unclickable).
  if (button == 0x110 && app.context_menu_open) {
    int cm_idx = hit_test_context_menu(app, x, y);
    if (cm_idx >= 0) {
      // Main menu item - if it has a submenu, just keep hover; otherwise execute
      if (static_cast<size_t>(cm_idx) < app.context_menu_items.size() &&
          !app.context_menu_items[cm_idx].sub_items.empty()) {
        app.context_menu_hover = cm_idx;
        draw(app);
        return;
      }
      execute_context_menu_action(app, cm_idx);
    } else if (cm_idx < -9) {
      // Submenu item: decode and execute
      int sub_idx = -(cm_idx + 10);
      int saved_parent = app.context_menu_hover_prev;
      app.context_menu_hover = -1; app.context_menu_hover_prev = -1; app.context_menu_sub_hover = -1;
      if (saved_parent >= 0 && static_cast<size_t>(saved_parent) < app.context_menu_items.size()) {
        auto& item = app.context_menu_items[saved_parent];
        if (sub_idx >= 0 && static_cast<size_t>(sub_idx) < item.sub_items.size()) {
          auto action = item.sub_items[sub_idx].action;
          if (action != AppState::ContextMenuAction::Separator) {
            // Clone the menu items to set file_idx for execute
            app.context_menu_open = false;
            // Rebuild a single-item context menu to reuse execute_context_menu_action
            auto saved_items = std::move(app.context_menu_items);
            app.context_menu_items = {item.sub_items[sub_idx]};
            execute_context_menu_action(app, 0);
            draw(app);
            if (app.context_menu_open) {
              app.context_menu_items = std::move(saved_items);
              app.context_menu_hover_prev = saved_parent;
            }
            return;
          }
        }
      }
      app.context_menu_open = false;
    } else {
      app.context_menu_open = false;
    }
    draw(app);
    return;
  }

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
      return;
    }
    app.active_pane = (x >= div_x + div_w) ? 1 : 0;
  }

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
      return;
    }
  }

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
        return;
      }
      if (app.status_zoom_plus[2] > 0 && in_rect(app.status_zoom_plus)) {
        step_zoom(app, +1);
        save_file_browser_settings(app);
        draw(app);
        return;
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
    return;
  }

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
        return;
      }
      if (x >= app.cancel_btn_x && x < app.cancel_btn_x + app.cancel_btn_w) {
        app.running = false;
        return;
      }
    }
  }

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
          return;
        }
      }
    }
  }

  // ── Sidebar drag start ──
  if (button == 0x110 && begin_sidebar_resize(app, x)) {
    return;
  }

  // ── Properties dialog clicks ──
  if (app.properties.open) {
    if (button == 0x110) {
      int hit = properties_hit_test(app, x, y);

      if (hit == -1 || hit == -2) {
        destroy_props_window(app);
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
        draw(app);
        return;
      }

      // Combo dropdown toggle
      if (hit >= 10 && hit <= 12) {
        int pi = hit - 10;
        if (app.properties.combo_open == pi)
          app.properties.combo_open = -1;
        else
          app.properties.combo_open = pi;
        draw(app);
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

        // Compute permission bits from combo values
        auto perm_bits = [](int level) -> mode_t {
          switch (level) {
            case 0: return 0;
            case 1: return S_IRUSR;
            case 2: return S_IRUSR | S_IWUSR;
            case 3: return S_IRUSR | S_IWUSR | S_IXUSR;
            default: return 0;
          }
        };
        mode_t mode = 0;
        mode |= perm_bits(app.properties.perm_owner) * (S_IRUSR | S_IWUSR | S_IXUSR) / (S_IRUSR | S_IWUSR | S_IXUSR);
        // Need per-user-group bit mapping
        mode = 0;
        mode |= (app.properties.perm_owner >= 1 ? S_IRUSR : 0);
        mode |= (app.properties.perm_owner >= 2 ? S_IWUSR : 0);
        mode |= (app.properties.perm_owner >= 3 ? S_IXUSR : 0);
        mode |= (app.properties.perm_group >= 1 ? S_IRGRP : 0);
        mode |= (app.properties.perm_group >= 2 ? S_IWGRP : 0);
        mode |= (app.properties.perm_group >= 3 ? S_IXGRP : 0);
        mode |= (app.properties.perm_other >= 1 ? S_IROTH : 0);
        mode |= (app.properties.perm_other >= 2 ? S_IWOTH : 0);
        mode |= (app.properties.perm_other >= 3 ? S_IXOTH : 0);

        // Preserve non-permission bits (setuid, setgid, sticky, etc.)
        mode |= (app.properties.current_mode & ~(S_IRWXU | S_IRWXG | S_IRWXO));

        if (app.properties.multi) {
          for (const auto& t : app.properties.paths) chmod(t.c_str(), mode);
        } else {
          chmod(app.properties.path.c_str(), mode);
        }
        app.properties.current_mode = mode;
        draw(app);
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
        draw(app);
        return;
      }

      // Clicked elsewhere inside dialog — close any open combo
      if (app.properties.combo_open >= 0) {
        app.properties.combo_open = -1;
        draw(app);
        return;
      }
    }
  }

  // ── Overwrite/merge conflict dialog clicks ──
  if (app.conflict_open) {
    if (button == 0x110) {
      // Checkbox
      const auto& cr = app.conflict_check_rect;
      if (app.pointerX >= cr[0] && app.pointerX < cr[0] + cr[2] &&
          app.pointerY >= cr[1] && app.pointerY < cr[1] + cr[3]) {
        app.conflict_apply_all = !app.conflict_apply_all;
        draw(app);
        return;
      }
      // Buttons: 0=Skip, 1=Cancel, 2=Overwrite/Merge
      for (int b = 0; b < 3; ++b) {
        const auto& r = app.conflict_btn_rects[b];
        if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) {
          resolve_conflict_choice(app, b);
          return;
        }
      }
      // Modal — ignore clicks elsewhere while open
    }
    return;
  }

  // ── Confirm dialog clicks ──
  if (app.confirm_open) {
    int dlg_w = 380;
    int dlg_h = 170;
    int dlg_x = (app.width - dlg_w) / 2;
    int dlg_y = (app.height - dlg_h) / 2;
    int cancel_x = dlg_x + dlg_w - 220;
    int delete_x = dlg_x + dlg_w - 110;
    int btn_y = dlg_y + dlg_h - 50;
    int btn_h = 32;
    int btn_w = 90;

    if (button == 0x110 && x >= delete_x && x < delete_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) {
      app.confirm_hover_btn = -1;
      if (app.confirm_callback) app.confirm_callback(true);
      app.confirm_open = false;
      draw(app);
      return;
    }

    if (button == 0x110 && ((x >= cancel_x && x < cancel_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) ||
        (x < dlg_x || x > dlg_x + dlg_w ||
         y < dlg_y || y > dlg_y + dlg_h))) {
      app.confirm_hover_btn = -1;
      if (app.confirm_callback) app.confirm_callback(false);
      app.confirm_open = false;
      draw(app);
      return;
    }
    return;
  }

  // ── Password dialog clicks ──
  if (app.password_dialog_open) {
    int card_w = 400;
    int card_h = 210;
    int cx = (app.width - card_w) / 2;
    int cy = (app.height - card_h) / 2;
    int pad = 24;
    int btn_h = 32;
    int btn_w = 90;
    int btn_gap = 10;
    int btns_total = btn_w * 2 + btn_gap;
    int btns_x = cx + (card_w - btns_total) / 2;
    int btn_y = cy + card_h - pad - btn_h;

    int extract_x = btns_x + btn_w + btn_gap;

    // Input field geometry (must match draw_password_dialog)
    int input_x = cx + pad;
    int input_y = cy + pad + 62;
    int input_w = card_w - pad * 2;
    int input_h = 36;

    // Right-click on input field → text context menu
    if (button == 0x111 && x >= input_x && x < input_x + input_w &&
        y >= input_y && y < input_y + input_h) {
      app.context_menu_open = true;
      app.context_menu_x = x;
      app.context_menu_y = y;
      app.context_menu_hover = -1; app.context_menu_hover_prev = -1; app.context_menu_sub_hover = -1;
      app.context_menu_file_idx = -8;
      app.context_menu_sidebar_idx = -1;
      bool has_sel = (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end);
      bool has_text = !app.password_buf.empty();
      app.context_menu_items = {};
      if (has_sel)
        app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Cut, "Cut"));
      if (has_sel)
        app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Copy, "Copy"));
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Paste, "Paste"));
      app.context_menu_items.push_back(AppState::menu_separator());
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::SelectAll, "Select All"));
      draw(app);
      return;
    }

    // Left-click on input field → position cursor + start drag
    if (button == 0x110 && x >= input_x && x < input_x + input_w &&
        y >= input_y && y < input_y + input_h) {
      std::string masked(app.password_buf.size(), '*');
      cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t* cr_tmp = cairo_create(tmp);
      cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr_tmp, 14);
      int click_x = x - input_x - 14;
      int best_pos = static_cast<int>(app.password_buf.size());
      for (int ci = 0; ci <= static_cast<int>(masked.size()); ++ci) {
        cairo_text_extents_t te;
        cairo_text_extents(cr_tmp, masked.substr(0, static_cast<std::size_t>(ci)).c_str(), &te);
        if (te.width >= click_x) { best_pos = ci; break; }
      }
      cairo_destroy(cr_tmp);
      cairo_surface_destroy(tmp);
      app.password_cursor_pos = best_pos;
      app.password_sel_start = -1;
      app.password_sel_end = -1;
      app.password_dragging = true;
      draw(app);
      return;
    }

    if (button == 0x110 && x >= extract_x && x < extract_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) {
      std::string arc = std::move(app.password_archive_path);
      std::string dst = std::move(app.password_dest_dir);
      std::string pw = std::move(app.password_buf);
      app.password_dialog_open = false;
      draw(app);
      execute_extract_with_password(app, arc, dst, pw);
      return;
    }

    if (button == 0x110 && ((x >= btns_x && x < btns_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) ||
        (x < cx || x > cx + card_w ||
         y < cy || y > cy + card_h))) {
      app.password_dialog_open = false;
      draw(app);
      return;
    }
    return;
  }

  // ── Compress dialog clicks ──
  if (app.compress_dialog_open) {
    int dlg_w = 420;
    int dlg_h = 310;
    int dlg_x = (app.width - dlg_w) / 2;
    int dlg_y = (app.height - dlg_h) / 2;
    int content_x = dlg_x + 20;
    int content_y = dlg_y + 50;
    int fmt_w = 80;
    int fmt_h = 28;
    int fmt_gap = 8;
    int fmy = content_y + 18;

    if (button == 0x110) {
      // Format buttons
      for (int i = 0; i < 4; ++i) {
        int fmx = content_x + i * (fmt_w + fmt_gap);
        if (x >= fmx && x < fmx + fmt_w && y >= fmy && y < fmy + fmt_h) {
          if (app.compress_format_available[i]) {
            app.compress_format = i;
            draw(app);
          }
          return;
        }
      }
      int fmy2 = fmy + fmt_h + fmt_gap;
      for (int i = 4; i < 7; ++i) {
        int fmx = content_x + (i - 4) * (fmt_w + fmt_gap);
        if (x >= fmx && x < fmx + fmt_w && y >= fmy2 && y < fmy2 + fmt_h) {
          if (app.compress_format_available[i]) {
            app.compress_format = i;
            draw(app);
          }
          return;
        }
      }

      // Level buttons
      int name_y = fmy2 + fmt_h + 14;
      int input_y = name_y + 18;
      int input_h = 32;
      int lvl_y = input_y + input_h + 14;
      int lvl_btn_y = lvl_y + 18;
      int lvl_btn_w = 68;
      int lvl_btn_h = 28;
      int lvl_gap = 8;
      static constexpr int kLevelValues[5] = {0, 3, 6, 8, 9};
      for (int i = 0; i < 5; ++i) {
        int lx = content_x + i * (lvl_btn_w + lvl_gap);
        if (x >= lx && x < lx + lvl_btn_w && y >= lvl_btn_y && y < lvl_btn_y + lvl_btn_h) {
          app.compress_level = kLevelValues[i];
          draw(app);
          return;
        }
      }

      // Bottom buttons
      int btn_y = dlg_y + dlg_h - 50;
      int btn_w = 90;
      int btn_h = 32;
      int cancel_x = dlg_x + dlg_w - 220;
      int compress_x = dlg_x + dlg_w - 110;

      if (x >= compress_x && x < compress_x + btn_w &&
          y >= btn_y && y < btn_y + btn_h) {
        app.compress_hover_btn = -1;
        execute_compress_async(app);
        return;
      }

      if ((x >= cancel_x && x < cancel_x + btn_w &&
           y >= btn_y && y < btn_y + btn_h) ||
          (x < dlg_x || x > dlg_x + dlg_w ||
           y < dlg_y || y > dlg_y + dlg_h)) {
        app.compress_dialog_open = false;
        draw(app);
        return;
      }
    }
    return;
  }

  // ── Select-by-pattern dialog clicks ──
  if (app.select_pattern_open) {
    if (handle_select_pattern_click(app, button, x, y)) {
      draw(app);
      return;
    }
  }

  // ── Create dialog clicks ──
  if (app.create_dialog_open) {
    int dlg_w = 340;
    int dlg_h = 160;
    int dlg_x = (app.width - dlg_w) / 2;
    int dlg_y = (app.height - dlg_h) / 2;

    int cancel_x = dlg_x + dlg_w - 220;
    int create_x = dlg_x + dlg_w - 110;
    int btn_y = dlg_y + dlg_h - 50;
    int btn_h = 32;
    int btn_w = 90;

    // Input field geometry (must match draw_create_dialog)
    int input_x = dlg_x + 20;
    int input_y = dlg_y + 50;
    int input_w = dlg_w - 40;
    int input_h = 34;

    // Right-click on input field → text context menu
    if (button == 0x111 && x >= input_x && x < input_x + input_w &&
        y >= input_y && y < input_y + input_h) {
      app.context_menu_open = true;
      app.context_menu_x = x;
      app.context_menu_y = y;
      app.context_menu_hover = -1; app.context_menu_hover_prev = -1; app.context_menu_sub_hover = -1;
      app.context_menu_file_idx = -6;
      app.context_menu_sidebar_idx = -1;
      bool has_sel = (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end);
      bool has_text = !app.create_buf.empty();
      app.context_menu_items = {};
      if (has_sel)
        app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Cut, "Cut"));
      if (has_sel)
        app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Copy, "Copy"));
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Paste, "Paste"));
      app.context_menu_items.push_back(AppState::menu_separator());
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::SelectAll, "Select All"));
      draw(app);
      return;
    }

    if (button == 0x110 && x >= create_x && x < create_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) {
      if (!app.create_buf.empty()) {
        fs::path dir(app.cur_tab().current_path);
        fs::path new_path = dir / app.create_buf;
        std::error_code ec;
        if (!app.create_template_src.empty()) {
          std::error_code eq;
          int n = 2;
          while (fs::exists(new_path, eq))
            new_path = dir / (new_path.stem().string() + " (" +
                              std::to_string(n++) + ")" +
                              new_path.extension().string());
          fs::copy_file(app.create_template_src, new_path,
                        fs::copy_options::none, ec);
        } else if (app.create_is_folder) {
          fs::create_directory(new_path, ec);
        } else {
          FILE* f = std::fopen(new_path.c_str(), "w");
          if (f) std::fclose(f);
        }
        reload_dir(app);
      }
      app.create_dialog_open = false;
      app.create_template_src.clear();
      draw(app);
      return;
    }

    if (button == 0x110 && x >= cancel_x && x < cancel_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) {
      app.create_dialog_open = false;
      app.create_template_src.clear();
      draw(app);
      return;
    }

    // Left-click on input field → position cursor + start drag
    if (button == 0x110 && x >= input_x && x < input_x + input_w &&
        y >= input_y && y < input_y + input_h) {
      cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t* cr_tmp = cairo_create(tmp);
      cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr_tmp, 14);
      int click_x = x - input_x - 10;
      int best_pos = static_cast<int>(app.create_buf.size());
      for (int ci = 0; ci <= static_cast<int>(app.create_buf.size()); ++ci) {
        cairo_text_extents_t te;
        cairo_text_extents(cr_tmp, app.create_buf.substr(0, static_cast<std::size_t>(ci)).c_str(), &te);
        if (te.width >= click_x) { best_pos = ci; break; }
      }
      cairo_destroy(cr_tmp);
      cairo_surface_destroy(tmp);
      app.create_cursor_pos = best_pos;
      app.create_sel_start = -1;
      app.create_sel_end = -1;
      app.create_dragging = true;
      draw(app);
      return;
    }

    if (button == 0x110 && (x < dlg_x || x > dlg_x + dlg_w ||
        y < dlg_y || y > dlg_y + dlg_h)) {
      app.create_dialog_open = false;
      app.create_template_src.clear();
      draw(app);
      return;
    }
    return;
  }

  // ── Rename UI dialog clicks ──
  if (app.rename_ui_open) {
    int dlg_w = 400;
    int dlg_h = 190;
    int dlg_x = (app.width - dlg_w) / 2;
    int dlg_y = (app.height - dlg_h) / 2;

    int cancel_x = dlg_x + dlg_w - 230;
    int rename_x = dlg_x + dlg_w - 120;
    int btn_y = dlg_y + dlg_h - 52;
    int btn_h = 34;
    int btn_w = 90;

    // Input field geometry (must match draw_rename_ui)
    int input_x = dlg_x + 24;
    int input_y = dlg_y + 64;
    int input_w = dlg_w - 48;
    int input_h = 36;

    // Right-click on input field → text context menu
    if (button == 0x111 && x >= input_x && x < input_x + input_w &&
        y >= input_y && y < input_y + input_h) {
      app.context_menu_open = true;
      app.context_menu_x = x;
      app.context_menu_y = y;
      app.context_menu_hover = -1; app.context_menu_hover_prev = -1; app.context_menu_sub_hover = -1;
      app.context_menu_file_idx = -7;
      app.context_menu_sidebar_idx = -1;
      bool has_sel = (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end);
      app.context_menu_items = {};
      if (has_sel)
        app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Cut, "Cut"));
      if (has_sel)
        app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Copy, "Copy"));
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Paste, "Paste"));
      app.context_menu_items.push_back(AppState::menu_separator());
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::SelectAll, "Select All"));
      draw(app);
      return;
    }

    // Left-click on input field → position cursor + start drag
    if (button == 0x110 && x >= input_x && x < input_x + input_w &&
        y >= input_y && y < input_y + input_h) {
      cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t* cr_tmp = cairo_create(tmp);
      cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr_tmp, 14);
      int click_x = x - input_x - 12;
      int best_pos = static_cast<int>(app.rename_ui_buf.size());
      for (int ci = 0; ci <= static_cast<int>(app.rename_ui_buf.size()); ++ci) {
        cairo_text_extents_t te;
        cairo_text_extents(cr_tmp, app.rename_ui_buf.substr(0, static_cast<std::size_t>(ci)).c_str(), &te);
        if (te.width >= click_x) { best_pos = ci; break; }
      }
      cairo_destroy(cr_tmp);
      cairo_surface_destroy(tmp);
      app.rename_ui_cursor_pos = best_pos;
      app.rename_ui_sel_start = -1;
      app.rename_ui_sel_end = -1;
      app.rename_ui_dragging = true;
      draw(app);
      return;
    }

    if (button == 0x110 && x >= rename_x && x < rename_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) {
      if (!app.rename_ui_buf.empty() && app.rename_ui_buf != app.rename_ui_old_name) {
        fs::path src(app.rename_ui_entry_path);
        fs::path dest = src.parent_path() / app.rename_ui_buf;
        std::error_code ec;
        fs::rename(src, dest, ec);
        if (!ec) {
          AppState::UndoRecord rec{AppState::UndoRecord::Type::Rename, {}, {}};
          rec.paths_a.push_back(src.string());
          rec.paths_b.push_back(dest.string());
          app.redo_stack.clear();
          app.undo_stack.push_back(std::move(rec));
          if (app.undo_stack.size() > app.kMaxUndo)
            app.undo_stack.erase(app.undo_stack.begin());
          app.operation_status = "Renamed";
          app.operation_status_expires_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::steady_clock::now() + std::chrono::milliseconds(3000)).time_since_epoch()).count();
          reload_dir(app);
        }
      }
      app.rename_ui_open = false;
      draw(app);
      return;
    }

    if (button == 0x110 && x >= cancel_x && x < cancel_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) {
      app.rename_ui_open = false;
      draw(app);
      return;
    }

    if (button == 0x110 && (x < dlg_x || x > dlg_x + dlg_w ||
        y < dlg_y || y > dlg_y + dlg_h)) {
      app.rename_ui_open = false;
      draw(app);
      return;
    }
    return;
  }

  // ── Batch rename click handling ──
  if (app.batch_rename_open) {
    int n = static_cast<int>(app.batch_rename_entries.size());
    bool is_template = (app.batch_rename_mode == 0);

    int dlg_w = 540;
    int list_h = std::min(n * 28 + 4, 280) + 4;
    int input_area_h = is_template ? 70 : 80;
    int dlg_h = 24 + 28 + input_area_h + list_h + 56;
    int dlg_x = (app.width - dlg_w) / 2;
    int dlg_y = (app.height - dlg_h) / 2;
    int cx = dlg_x + 20;

    int tab_y = dlg_y + 42;
    int tab_h = 26;
    int tab_w = 210;
    int input_y = tab_y + tab_h + 10;
    int field_h = 30;
    int btn_y = dlg_y + dlg_h - 44;
    int btn_w = 90;
    int btn_h = 32;
    int cancel_x = dlg_x + dlg_w - 230;
    int rename_x = dlg_x + dlg_w - 120;

    if (button == 0x110) {
      // ── Mode tab clicks ──
      if (y >= tab_y && y < tab_y + tab_h) {
        if (x >= cx && x < cx + tab_w) {
          if (app.batch_rename_mode != 0) {
            app.batch_rename_mode = 0;
            app.batch_rename_edit_focus = 0;
            app.batch_rename_show_add = false;
            draw(app);
          }
          return;
        }
        if (x >= cx + tab_w + 8 && x < cx + tab_w + 8 + tab_w) {
          if (app.batch_rename_mode != 1) {
            app.batch_rename_mode = 1;
            app.batch_rename_edit_focus = 0;
            draw(app);
          }
          return;
        }
      }

      if (is_template) {
        // ── Template field click ──
        int tf_x = cx;
        int tf_y = input_y;
        int tf_w = 360;
        if (x >= tf_x && x < tf_x + tf_w && y >= tf_y && y < tf_y + field_h) {
          app.batch_rename_edit_focus = 0;
          app.batch_rename_show_add = false;
          // Position cursor based on click
          app.batch_rename_template_cursor = static_cast<int>(app.batch_rename_template.size());
          draw(app);
          return;
        }

        // ── [+ Add] button click ──
        int add_x = tf_x + tf_w + 8;
        int add_w = 70;
        if (x >= add_x && x < add_x + add_w && y >= tf_y && y < tf_y + field_h) {
          app.batch_rename_show_add = !app.batch_rename_show_add;
          app.batch_rename_add_hover = -1;
          draw(app);
          return;
        }

        // ── [+ Add] dropdown option click ──
        if (app.batch_rename_show_add) {
          int dd_x = add_x;
          int dd_y = tf_y + field_h + 2;
          int dd_w = add_w;
          int dd_item_h = 26;
          int dd_h = 3 * dd_item_h + 4;
          if (x >= dd_x && x < dd_x + dd_w && y >= dd_y && y < dd_y + dd_h) {
            int option = (y - dd_y - 2) / dd_item_h;
            if (option >= 0 && option <= 2) {
              const char* inserts[] = {"[1]", "[01]", "[001]"};
              app.batch_rename_template.insert(app.batch_rename_template_cursor, inserts[option]);
              app.batch_rename_template_cursor += static_cast<int>(std::strlen(inserts[option]));
              app.batch_rename_show_add = false;
              draw(app);
            }
            return;
          }
          // Click outside dropdown closes it
          if (!(x >= dd_x && x < dd_x + dd_w && y >= dd_y && y < dd_y + dd_h)) {
            app.batch_rename_show_add = false;
            draw(app);
          }
        }
      } else {
        // ── Find mode field clicks ──
        int label_w = 100;
        int fld_x = cx + label_w;
        int fld_w = 240;

        // Find field
        if (x >= fld_x && x < fld_x + fld_w && y >= input_y && y < input_y + field_h) {
          app.batch_rename_edit_focus = 0;
          app.batch_rename_find_cursor = static_cast<int>(app.batch_rename_find.size());
          draw(app);
          return;
        }

        // Replace field
        int rl_y = input_y + field_h + 6;
        if (x >= fld_x && x < fld_x + fld_w && y >= rl_y && y < rl_y + field_h) {
          app.batch_rename_edit_focus = 1;
          app.batch_rename_replace_cursor = static_cast<int>(app.batch_rename_replace.size());
          draw(app);
          return;
        }
      }

      // ── Rename button ──
      if (x >= rename_x && x < rename_x + btn_w && y >= btn_y && y < btn_y + btn_h) {
        if (!app.batch_rename_entries.empty()) {
          AppState::UndoRecord rec{AppState::UndoRecord::Type::Rename, {}, {}};
          std::error_code ec;
          int renamed = 0;
          for (const auto& e : app.batch_rename_entries) {
            if (e.new_name.empty() || e.new_name == e.old_name) continue;
            fs::path src(e.old_path);
            fs::path dest = src.parent_path() / e.new_name;
            fs::rename(src, dest, ec);
            if (!ec) {
              rec.paths_a.push_back(e.old_path);
              rec.paths_b.push_back(dest.string());
              ++renamed;
            }
          }
          if (!rec.paths_a.empty()) {
            app.redo_stack.clear();
            app.undo_stack.push_back(std::move(rec));
            if (app.undo_stack.size() > app.kMaxUndo)
              app.undo_stack.erase(app.undo_stack.begin());
            app.operation_status = std::to_string(renamed) + " file" + (renamed == 1 ? "" : "s") + " renamed";
            app.operation_status_expires_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              (std::chrono::steady_clock::now() + std::chrono::milliseconds(3000)).time_since_epoch()).count();
            reload_dir(app);
          }
        }
        app.batch_rename_open = false;
        draw(app);
        return;
      }

      // ── Cancel button or click outside ──
      if ((x >= cancel_x && x < cancel_x + btn_w && y >= btn_y && y < btn_y + btn_h) ||
          (x < dlg_x || x > dlg_x + dlg_w || y < dlg_y || y > dlg_y + dlg_h)) {
        app.batch_rename_open = false;
        draw(app);
        return;
      }
    }
    return;
  }

  // ── Open With dialog helper: write MIME default association ──
  auto set_mime_default_app = [](const std::string& mime_type, const std::string& desktop_id) {
    if (mime_type.empty() || desktop_id.empty()) return;
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/.config/mimeapps.list";

    // Read existing file
    std::vector<std::string> lines;
    bool in_defaults = false;
    bool found = false;
    {
      FILE* f = fopen(path.c_str(), "r");
      if (f) {
        char buf[1024];
        std::string prefix = mime_type + "=";
        while (fgets(buf, sizeof(buf), f)) {
          std::string line = buf;
          if (!line.empty() && line.back() == '\n') line.pop_back();
          if (line == "[Default Applications]") { in_defaults = true; lines.push_back(line); continue; }
          if (!line.empty() && line[0] == '[') { in_defaults = false; }
          if (in_defaults && line.size() >= prefix.size() &&
              line.compare(0, prefix.size(), prefix) == 0) {
            std::string existing = line.substr(prefix.size());
            if (existing.find(desktop_id) == std::string::npos) {
              line = prefix + existing + ";" + desktop_id;
            }
            found = true;
          }
          lines.push_back(line);
        }
        fclose(f);
      }
    }

    if (!found) {
      // Ensure [Default Applications] section exists
      bool has_section = false;
      for (const auto& l : lines) {
        if (l == "[Default Applications]") { has_section = true; break; }
      }
      if (!has_section) {
        if (!lines.empty()) lines.push_back("");
        lines.push_back("[Default Applications]");
      }
      // Append at end (after [Default Applications] header or at file end)
      lines.push_back(mime_type + "=" + desktop_id);
    }

    FILE* f = fopen(path.c_str(), "w");
    if (f) {
      for (const auto& l : lines) {
        fprintf(f, "%s\n", l.c_str());
      }
      fclose(f);
    }
  };

  // ── Open With dialog clicks ──
  if (app.open_with_open) {
    if (button == 0x110) {
      double dx = static_cast<double>(x), dy = static_cast<double>(y);

      // Outside card → close
      if (dx < app.open_with_x || dx > app.open_with_x + app.open_with_w ||
          dy < app.open_with_y || dy > app.open_with_y + app.open_with_h) {
        open_with_close(app);
        draw(app);
        return;
      }

      // Close button
      if (dx >= app.open_with_hit_close[0] && dx < app.open_with_hit_close[0] + app.open_with_hit_close[2] &&
          dy >= app.open_with_hit_close[1] && dy < app.open_with_hit_close[1] + app.open_with_hit_close[3]) {
        open_with_close(app);
        draw(app);
        return;
      }

      // Cancel button
      if (dx >= app.open_with_hit_cancel[0] && dx < app.open_with_hit_cancel[0] + app.open_with_hit_cancel[2] &&
          dy >= app.open_with_hit_cancel[1] && dy < app.open_with_hit_cancel[1] + app.open_with_hit_cancel[3]) {
        open_with_close(app);
        draw(app);
        return;
      }

      // "Set as Default" toggle
      if (dx >= app.open_with_hit_default[0] && dx < app.open_with_hit_default[0] + app.open_with_hit_default[2] &&
          dy >= app.open_with_hit_default[1] && dy < app.open_with_hit_default[1] + app.open_with_hit_default[3]) {
        app.open_with_set_default = !app.open_with_set_default;
        draw(app);
        return;
      }

      auto launch = [&](const AppState::OpenWithEntry& e) {
        std::string desktop = e.desktop_path;
        std::string file = app.open_with_file_path;
        for (size_t p = 0; (p = desktop.find('\'', p)) != std::string::npos; p += 4)
          desktop.replace(p, 1, "'\\''");
        for (size_t p = 0; (p = file.find('\'', p)) != std::string::npos; p += 4)
          file.replace(p, 1, "'\\''");
        std::string cmd = "gio launch '" + desktop + "' '" + file + "' &";
        (void)std::system(cmd.c_str());
      };

      // Open button
      if (dx >= app.open_with_hit_open[0] && dx < app.open_with_hit_open[0] + app.open_with_hit_open[2] &&
          dy >= app.open_with_hit_open[1] && dy < app.open_with_hit_open[1] + app.open_with_hit_open[3]) {
        if (app.open_with_selected >= 0 &&
            app.open_with_selected < static_cast<int>(app.open_with_apps.size())) {
          if (app.open_with_set_default) {
            set_mime_default_app(app.open_with_mime, app.open_with_apps[app.open_with_selected].desktop_id);
          }
          launch(app.open_with_apps[app.open_with_selected]);
        }
        open_with_close(app);
        draw(app);
        return;
      }

      // App list
      int pad = 16, pad_in = 12, top_bar_h = 44, entry_h = 40, section_h = 26;
      int total = static_cast<int>(app.open_with_apps.size());
      int rec_count = app.open_with_exact_count;
      int total_content_h = total * entry_h;
      if (rec_count > 0) total_content_h += section_h;
      if (rec_count < total) total_content_h += section_h;
      int list_h = std::min(total_content_h, 320);
      int list_x = static_cast<int>(app.open_with_x) + pad_in;
      int list_y = static_cast<int>(app.open_with_y) + pad + top_bar_h + pad_in;
      int list_w = static_cast<int>(app.open_with_w) - 2 * pad_in;

      if (dx >= list_x && dx < list_x + list_w &&
          dy >= list_y && dy < list_y + list_h) {
        int content_y = app.open_with_scroll + static_cast<int>(dy - list_y);
        int cy_off = 0;
        for (int i = 0; i < total; ++i) {
          if (i == 0 && rec_count > 0) cy_off += section_h;
          if (i == rec_count && rec_count < total) cy_off += section_h;
          if (content_y >= cy_off && content_y < cy_off + entry_h) {
            app.open_with_selected = i;
            if (app.open_with_set_default) {
              set_mime_default_app(app.open_with_mime, app.open_with_apps[i].desktop_id);
            }
            launch(app.open_with_apps[i]);
            open_with_close(app);
            draw(app);
            return;
          }
          cy_off += entry_h;
        }
      }
    }
    return;
  }

  // ── Terminal chooser clicks ──
  if (app.term_chooser_open) {
    const int kPad = 20, kTopBarH = 44, kEntryH = 40, kBottomBarH = 52;
    const int kMaxListH = 300;
    const int total = static_cast<int>(app.term_chooser_apps.size());
    const int max_visible = std::max(1, kMaxListH / kEntryH);
    const int visible = std::min(total, max_visible);
    const int list_h = visible * kEntryH;
    const int card_w = app.term_chooser_w;
    const int card_h = kPad + kTopBarH + 8 + list_h + 8 + kBottomBarH + kPad;
    const int card_x = app.term_chooser_x;
    const int card_y = (app.height - card_h) / 2;

    const int close_x = card_x + card_w - kPad - 28;
    const int close_y = card_y + kPad - 4;
    const int list_x = card_x + 12;
    const int list_y = card_y + kPad + kTopBarH + 8;

    if (button == 0x110) {
      if (x < card_x || x > card_x + card_w || y < card_y || y > card_y + card_h) {
        app.term_chooser_open = false;
        draw(app);
        return;
      }
      if (x >= close_x && x < close_x + 28 && y >= close_y && y < close_y + 28) {
        app.term_chooser_open = false;
        draw(app);
        return;
      }
      if (x >= list_x && x < list_x + card_w - 24 && y >= list_y && y < list_y + list_h) {
        int rel_y = y - list_y + app.term_chooser_scroll * kEntryH;
        int item_idx = rel_y / kEntryH;
        if (item_idx >= 0 && item_idx < total) {
          auto& chosen = app.term_chooser_apps[item_idx];
          std::string chosen_id = chosen.desktop_id;
          auto dot = chosen_id.rfind('.');
          if (dot != std::string::npos) chosen_id = chosen_id.substr(0, dot);
          auto slash = chosen_id.rfind('/');
          if (slash != std::string::npos) chosen_id = chosen_id.substr(slash + 1);
          eh::config::ShellConfig sc = eh::config::shell_config_snapshot();
          sc.defaultApps.terminal = chosen_id;
          (void)eh::config::write_state_settings_toml(sc);
          eh::config::shell_config_apply_from_memory(std::move(sc));
          app.term_chooser_open = false;
          open_terminal_at(app, app.term_chooser_target_dir);
          draw(app);
          return;
        }
      }
    }
    return;
  }

  // Left click
  if (button == 0x110) {
    // Prefer the compositor's event timestamp: UI-thread stalls (e.g. a slow
    // frame between the two clicks of a double-click) must not inflate the
    // apparent gap and reject genuine double-clicks.
    uint64_t now_ns = 0;
    {
      uint32_t evt_ms = app.seat.last_pointer_button_time_ms();
      if (evt_ms != 0) {
        now_ns = static_cast<uint64_t>(evt_ms) * 1000000ull;
      } else {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                 static_cast<uint64_t>(ts.tv_nsec);
      }
    }

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
        return;
      } else {
        cmo = false;
        draw(app);
        return;
      }
    }

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
          return;
        }
      } else {
        (app.active_pane ? app.r_sort_menu_open : app.sort_menu_open) = false;
        draw(app);
        return;
      }
    }

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
          return;
        } else if (clicked_section > 0) {
          // Header clicked — toggle expansion
          click_filter_section = (click_filter_section == clicked_section) ? 0 : clicked_section;
          click_filter_hover = -1;
          draw(app);
          return;
        }
      } else {
        click_filter_section = 0;
        draw(app);
        return;
      }
    }

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
          return;
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
          return;
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
        return;
      }

      // Navigation arrows (back, forward, up)
      int bx = app.nav_origin_x();
      int btn_w = static_cast<int>(36.0 * zf);
      int gap4 = static_cast<int>(6.0 * zf);
      // Sidebar fold toggle (drawn before the arrows when folded)
      if (sidebar_toggle_hit(app, x, y)) {
        toggle_sidebar_flap(app);
        draw(app);
        return;
      }
      if (x >= bx && x < bx + btn_w && !app.cur_tab().nav_history.empty()) {
        navigate_back(app);
        draw(app);
        return;
      }
      bx += btn_w + gap4;
      if (x >= bx && x < bx + btn_w && !app.cur_tab().nav_forward.empty()) {
        navigate_forward(app);
        draw(app);
        return;
      }
      bx += btn_w + gap4;
      if (x >= bx && x < bx + btn_w && can_navigate_up(app)) {
        navigate_up(app);
        draw(app);
        return;
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
      return;
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
        return;
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
        return;
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
        return;
      }

      // Filter button click (only when search is active)
      if (in_search_active || in_recursive_search_active) {
        if (x >= in_filter_btn_x && x < in_filter_btn_x + in_filter_btn_w) {
          auto& click_filter_section = app.active_pane ? app.r_filter_dropdown_section : app.filter_dropdown_section;
          auto& click_filter_hover = app.active_pane ? app.r_filter_dropdown_hover : app.filter_dropdown_hover;
          click_filter_section = (click_filter_section > 0) ? 0 : 1;
          click_filter_hover = -1;
          draw(app);
          return;
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
            return;
          }
          if (cb_case_w > 0 && x >= cb_case_x && x < cb_case_x + cb_case_w) {
            cb_case = !cb_case;
            restart_active_search(app);
            draw(app);
            return;
          }
          if (cb_lock_w > 0 && x >= cb_lock_x && x < cb_lock_x + cb_lock_w) {
            cb_lock = !cb_lock;
            draw(app);
            return;
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
          return;
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
          return;
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
          return;
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
        return;
      }

      // Breadcrumb click
      for (size_t i = 0; i < in_breadcrumbs.size(); ++i) {
        auto& seg = in_breadcrumbs[i];
        if (x >= seg.x && x < seg.x + seg.w) {
          navigate_to(app, seg.path);
          draw(app);
          return;
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
      return;
    }

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
              return;
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
            return;
          }
          if (button == 0x210 && i < app.tabs.size()) {
            // Middle-click on tab → close
            app.active_tab = static_cast<int>(i);
            close_tab(app);
            draw(app);
            return;
          }
          if (button == 0x210 && i < app.tabs.size()) {
            // Middle-click on tab → close (only reached for left-clicks due to outer scope)
            app.active_tab = static_cast<int>(i);
            close_tab(app);
            draw(app);
            return;
          }
        }
      }
      return;
    }

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
        return;
      }
    }

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
        return;
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
      return;
    }

    // ── Flap swallow ──
    // Inside the revealed overlay but not on a sidebar item: swallow the click
    // so it doesn't act on the content hidden beneath the flap.
    if (app.sidebar_folded && app.sidebar_folded_revealed && button == 0x110 &&
        x < app.effective_sidebar_width() && y >= app.content_top_y()) {
      draw(app);
      return;
    }

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
        return;
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
        draw(app); return;
      }
      if (std::abs(x - x2) < 4) {
        app.col_resizing = 1;
        app.col_resize_start_frac = static_cast<double>(x - s_w) /
                                     static_cast<double>(std::max(1, content_w));
        draw(app); return;
      }
      if (std::abs(x - x3) < 4) {
        app.col_resizing = 2;
        app.col_resize_start_frac = static_cast<double>(x - s_w) /
                                     static_cast<double>(std::max(1, content_w));
        draw(app); return;
      }

      SortField clicked = SortField::Name;
      if (x >= text_x && x < x1) clicked = SortField::Name;
      else if (x >= x1 && x < x2) clicked = SortField::Size;
      else if (x >= x2 && x < x3) clicked = SortField::Modified;
      else if (x >= x3) clicked = SortField::Type;
      else { draw(app); return; }

      if (app.cur_tab().sort_field == clicked)
        app.cur_tab().sort_descending = !app.cur_tab().sort_descending;
      else {
        app.cur_tab().sort_field = clicked;
        app.cur_tab().sort_descending = false;
      }
      reload_dir(app);
      draw(app);
      return;
    }

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
      return;
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
          return;
        }
        uint64_t elapsed_ns = now_ns - app.last_click_ns;
        bool same_pos = std::abs(x - app.last_click_x) < 8 &&
                        std::abs(y - app.last_click_y) < 8;
        if (idx >= 0 && idx == app.last_click_idx && same_pos && elapsed_ns < 400000000ull) {
          // Double-click
          if (item.shape == ComputerItem::ShapeType::Small && !item.path.empty()) {
            navigate_to(app, item.path);
            app.last_click_ns = 0;
            return;
          } else if (item.shape == ComputerItem::ShapeType::Large && item.is_mounted && !item.path.empty()) {
            navigate_to(app, item.path);
            app.last_click_ns = 0;
            return;
          }
          app.last_click_ns = 0;
          draw(app);
          return;
        }
        // Single click: select
        app.computer_hover_idx = idx;
        app.last_click_ns = now_ns;
        app.last_click_x = x;
        app.last_click_y = y;
        app.last_click_idx = idx;
        draw(app);
        return;
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
        return;
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
    return;
  }

  // Right click (BTN_RIGHT = 0x111)
  if (button == 0x111) {
    // Path editing right-click context menu
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
      return;
    }
    }

    // Tab bar right-click context menu
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
          return;
        }
      }
      return;
    }

    // Check Computer view right-click for drive mount/unmount
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
          return;
        }
      }
    }

    if (app.context_menu_open) {
      app.context_menu_open = false;
      draw(app);
      return;
    }

    // ── Operations panel cancel (right-click too) ──
    if (app.ops_panel_open && app.ops_cancel_w > 0) {
      if (x >= app.ops_cancel_x && x < app.ops_cancel_x + app.ops_cancel_w &&
          y >= app.ops_cancel_y && y < app.ops_cancel_y + app.ops_cancel_h) {
        if (app.op_progress) app.op_progress->cancel = true;
        app.operation_status = "Cancelling...";
        app.operation_status_expires_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() + 3000;
        draw(app);
        return;
      }
    }

    // Check sidebar right-click first (sidebar items get context menu)
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
      return;
    }

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
    return;
  }
}


} // namespace eh::file_browser
