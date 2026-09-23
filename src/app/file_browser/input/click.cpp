// click.cpp — click hit-testing and the click handler.
// handle_click, properties_hit_test / settings_hit_test and the modal
// gate moved wholesale from events.cpp (byte-identical).
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

// handle_click dispatches to per-region handlers (click_dialogs.cpp,
// click_chrome.cpp, click_views.cpp). Regions were carved at the ── region
// seams; each bool handler returns true exactly where the original
// returned. The modal gate helper moved to click_chrome.cpp.
void handle_click(AppState& app, int x, int y, int button) {
  hide_tooltip(app);
  app.pointerX = static_cast<double>(x);
  app.pointerY = static_cast<double>(y);

  // ── Adaptive sidebar flap: clicks outside dismiss it ──
  // The flap is a transient overlay, so clicking anywhere outside the flap
  // closes it. The click is still processed normally (it acts on whatever
  // was clicked), matching the Nautilus AdwFlap behavior.
  dismiss_sidebar_flap(app, x, y);

  if (click_drop_chooser(app, x, y, button)) return;
  if (click_context_menu(app, x, y, button)) return;
  if (click_split_pane(app, x, y, button)) return;
  if (click_scrollbar(app, x, y, button)) return;
  if (click_path_edit_cancel(app, x, y, button)) return;
  if (click_status_zoom(app, x, y, button)) return;
  if (click_picker_bar(app, x, y, button)) return;
  if (click_info_tab(app, x, y, button)) return;
  if (click_sidebar_drag(app, x, y, button)) return;
  if (click_properties(app, x, y, button)) return;
  if (click_conflict(app, x, y, button)) return;
  if (click_confirm(app, x, y, button)) return;
  if (click_password(app, x, y, button)) return;
  if (click_compress(app, x, y, button)) return;
  if (click_select_pattern(app, x, y, button)) return;
  if (click_create(app, x, y, button)) return;
  if (click_rename_ui(app, x, y, button)) return;
  if (click_batch_rename(app, x, y, button)) return;
  if (click_open_with(app, x, y, button)) return;
  if (click_term_chooser(app, x, y, button)) return;

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

    if (click_columns_menu(app, x, y, button)) return;
    if (click_sort_menu(app, x, y, button)) return;
    if (click_filter_dropdown(app, x, y, button)) return;
    if (click_top_bar(app, x, y, button)) return;
    if (click_tab_bar(app, x, y, button)) return;
    if (click_ops_cancel(app, x, y, button)) return;
    if (click_sidebar_hit(app, x, y, button)) return;
    if (click_flap_swallow(app, x, y, button)) return;
    if (click_column_header(app, x, y, button)) return;
    if (click_content_hit(app, x, y, button, now_ns)) return;
    return;
  }

  if (button == 0x111) {
    if (click_rpath_edit(app, x, y, button)) return;
    if (click_rtab_bar(app, x, y, button)) return;
    if (click_rcomputer(app, x, y, button)) return;
    if (click_rctx_close(app, x, y, button)) return;
    if (click_ops_cancel(app, x, y, button)) return;
    if (click_rsidebar(app, x, y, button)) return;
    if (click_rcontent(app, x, y, button)) return;
  }
}


} // namespace eh::file_browser
