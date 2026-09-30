// keyboard.cpp — handle_key dispatcher + the grouped-header row helpers used
// to compute keyboard target rows. Moved from events.cpp. The key handler is
// further split into per-region handlers (key_dialog.cpp, key_edit.cpp,
// key_shortcuts.cpp, key_nav.cpp); handle_key keeps the prologue, the
// split-focus block, and the shared show_hidden_passthrough computation and
// dispatches to them in flow order.
// NOTE: headers_before/header_h/entry_top/entry_bottom live here even
// though they sit next to handle_scroll in the source — they are only
// consumed by handle_key.
#include "events.hpp"
#include "../app.hpp"
#include "../ui/layout.hpp"
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
bool handle_key(AppState& app, uint32_t, uint32_t state,
                xkb_keysym_t sym, const char* utf8, int utf8_len) {
  // Key release — clear repeat tracking
  if (state == 0) {
    if (sym == app.key_repeat_sym) app.key_repeat_sym = 0;
    return false;
  }
  if (state != 1) return false;

  auto* xkb = app.seat.xkb_state_ptr();
  bool ctrl = xkb && xkb_state_mod_name_is_active(xkb, XKB_MOD_NAME_CTRL,
                                                   XKB_STATE_MODS_EFFECTIVE) != 0;
  bool shift = xkb && xkb_state_mod_name_is_active(xkb, XKB_MOD_NAME_SHIFT,
                                                    XKB_STATE_MODS_EFFECTIVE) != 0;
  bool alt = xkb && xkb_state_mod_name_is_active(xkb, XKB_MOD_NAME_ALT,
                                                  XKB_STATE_MODS_EFFECTIVE) != 0;

  if (key_properties(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  // In split view, determine which pane has keyboard focus
  if (app.split_view) {
    if (app.r_path_editing || app.r_search_active || app.r_recursive_search_active)
      app.active_pane = 1;
    else if (app.path_editing || app.search_active || app.recursive_search_active)
      app.active_pane = 0;
  }

  if (key_cancel_op(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (key_confirm(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (app.du_open && app.focused_surface == app.du_surface) {
    if (handle_du_key(app, sym, ctrl)) return true;
    return true; // swallow everything else on this surface
  }

  if (key_settings(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (key_select_pattern(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (key_compress(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (key_create(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;
  if (key_password(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;
  if (key_rename_ui(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;
  if (key_batch_rename(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;
  if (key_term_chooser(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;
  if (key_checksum(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;
  if (key_connect(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;
  if (key_remote_auth(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  // Ctrl+H falls through so the global show-hidden toggle keeps working
  const bool show_hidden_passthrough =
      ctrl && !shift && !alt && (sym == XKB_KEY_H || sym == XKB_KEY_h);
  if (key_search(app, sym, ctrl, shift, alt, utf8, utf8_len,
                 show_hidden_passthrough)) return true;

  // Ctrl+H falls through so the global show-hidden toggle keeps working
  if (key_path_edit(app, sym, ctrl, shift, alt, utf8, utf8_len,
                    show_hidden_passthrough)) return true;

  if (key_global_shortcuts(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (key_navigate(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (key_text_fallback(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (key_type_to_find(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (key_space_dismiss(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (key_drop_dismiss(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  if (key_escape_clear(app, sym, ctrl, shift, alt, utf8, utf8_len)) return true;

  return false;
}


} // namespace eh::file_browser
