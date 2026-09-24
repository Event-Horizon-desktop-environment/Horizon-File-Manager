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
  // Resolved through the retained hit registry (rects stored during paint
  // in the settings window's coordinate space); no geometry is re-derived
  // here. Return codes are unchanged so handle_settings_click is untouched.
  const uint32_t hid = app.hit_settings.query(x, y);
  if ((hid & hui::Hit::kGroupMask) != hui::Hit::kDialog) return -1;
  if ((hid & 0xFFFFC00) != hui::Hit::dialog(hui::Hit::kDlgSettings, 0)) return -1;
  const int ctrl = hui::Hit::dialog_ctrl(hid);
  switch (ctrl) {
    case hui::Hit::kSettingsClose: return -2;
    case hui::Hit::kSettingsTabBase + 0: return -3;
    case hui::Hit::kSettingsTabBase + 1: return -4;
    case hui::Hit::kSettingsTabBase + 2: return -22;
    case hui::Hit::kSettingsOk: return -5;
    case hui::Hit::kSettingsApply: return -6;
    case hui::Hit::kSettingsCancel: return -7;
    case hui::Hit::kSettingsZoomDown: return -8;
    case hui::Hit::kSettingsZoomUp: return -9;
    case hui::Hit::kSettingsZoomField: return -16;
    case hui::Hit::kSettingsFoldersToggle: return -10;
    case hui::Hit::kSettingsSurfSlider: return -11;
    case hui::Hit::kSettingsSideSlider: return -13;
    case hui::Hit::kSettingsTopSlider: return -14;
    case hui::Hit::kSettingsStatusSlider: return -15;
    case hui::Hit::kSettingsPrevSlider: return -17;
    case hui::Hit::kSettingsDlgSlider: return -19;
    case hui::Hit::kSettingsPropsSlider: return -20;
    case hui::Hit::kSettingsScaleSlider: return -23;
    case hui::Hit::kSettingsMatugen: return -18;
    case hui::Hit::kSettingsColorEng: return -24;
    case hui::Hit::kSettingsTermDrop: return -12;
    case hui::Hit::kSettingsIndepToggle: return -21;
    default: break;
  }
  if (ctrl >= hui::Hit::kSettingsDropItemBase &&
      ctrl < hui::Hit::kSettingsDropItemBase + 6)
    return ctrl - hui::Hit::kSettingsDropItemBase;
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
  // was clicked), matching the flap overlay behavior.
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
    if (click_top_bar(app, x, y, button, now_ns)) return;
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
