// keyboard.cpp — key handler and the grouped-header row helpers used
// to compute keyboard target rows. Moved from events.cpp.
// NOTE: headers_before/header_h/entry_top/entry_bottom live here even
// though they sit next to handle_scroll in the source — they are only
// consumed by handle_key.
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
// ── grouped-header row helpers + key handler (moved from events.cpp) ──────────────────────────
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

  // ── Properties window keyboard (octal editor / tags editor / Escape) ──
  if (app.properties.open && app.focused_surface == app.props_surface) {
    auto& pr = app.properties;
    if (pr.octal_edit) {
      if (sym == XKB_KEY_Escape) {
        pr.octal_edit = false;
        app.props_pendingRedraw = true;
        draw(app);
        return true;
      }
      if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        char* end = nullptr;
        const unsigned long parsed =
            std::strtoul(pr.octal_buf.c_str(), &end, 8);
        if (end && *end == '\0' && !pr.octal_buf.empty() && parsed <= 07777ul) {
          const mode_t mode = static_cast<mode_t>(parsed);
          if (pr.multi) {
            for (const auto& t : pr.paths) ::chmod(t.c_str(), mode);
          } else {
            ::chmod(pr.path.c_str(), mode);
          }
          pr.current_mode = mode;
          // Resync the coarse permission combos with the new mode
          auto perm_level = [](mode_t bits) {
            const bool r = bits & 4, w = bits & 2, x = bits & 1;
            if (!r) return 0;
            if (!w) return 1;
            if (!x) return 2;
            return 3;
          };
          pr.perm_owner = perm_level((mode & 0700) >> 6);
          pr.perm_group = perm_level((mode & 0070) >> 3);
          pr.perm_other = perm_level(mode & 0007);
          pr.executable = (mode & 0111) != 0;
          pr.octal_edit = false;
        }
        app.props_pendingRedraw = true;
        draw(app);
        return true;
      }
      if (sym == XKB_KEY_BackSpace) {
        if (!pr.octal_buf.empty()) pr.octal_buf.pop_back();
        app.props_pendingRedraw = true;
        draw(app);
        return true;
      }
      if (utf8_len > 0) {
        const char c = utf8[0];
        if (c >= '0' && c <= '7' && pr.octal_buf.size() < 4) {
          pr.octal_buf += c;
          app.props_pendingRedraw = true;
          draw(app);
        }
        return true;
      }
      return true; // swallow everything else while editing
    }
    if (pr.tags_edit) {
      if (sym == XKB_KEY_Escape) {
        pr.tags_edit = false;
        app.props_pendingRedraw = true;
        draw(app);
        return true;
      }
      if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        write_xdg_tags(pr.path, pr.tags_buf);
        pr.tags_value = read_xdg_tags(pr.path);
        pr.tags_edit = false;
        app.props_pendingRedraw = true;
        draw(app);
        return true;
      }
      if (sym == XKB_KEY_BackSpace) {
        // UTF-8 aware: pop continuation bytes then the lead byte
        while (!pr.tags_buf.empty() &&
               (static_cast<unsigned char>(pr.tags_buf.back()) & 0xC0) == 0x80)
          pr.tags_buf.pop_back();
        if (!pr.tags_buf.empty()) pr.tags_buf.pop_back();
        app.props_pendingRedraw = true;
        draw(app);
        return true;
      }
      if (utf8_len > 0 && utf8_len <= 4) {
        const unsigned char c0 = static_cast<unsigned char>(utf8[0]);
        if (c0 >= 0x20 && c0 != 0x7f && pr.tags_buf.size() + static_cast<size_t>(utf8_len) < 1024) {
          pr.tags_buf.append(utf8, static_cast<size_t>(utf8_len));
          app.props_pendingRedraw = true;
          draw(app);
        }
        return true;
      }
      return true; // swallow everything else while editing
    }
    if (sym == XKB_KEY_Escape) {
      destroy_props_window(app);
      draw(app);
      return true;
    }
    return true; // properties window focused: don't leak keys to the view
  }

  // In split view, determine which pane has keyboard focus
  if (app.split_view) {
    if (app.r_path_editing || app.r_search_active || app.r_recursive_search_active)
      app.active_pane = 1;
    else if (app.path_editing || app.search_active || app.recursive_search_active)
      app.active_pane = 0;
  }

  // ── Cancel running operation ──
  if (sym == XKB_KEY_Escape && app.op_progress && app.op_progress->active) {
    app.op_progress->cancel = true;
    app.operation_status = "Cancelling...";
    app.operation_status_expires_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() + 3000;
    draw(app);
    return true;
  }

  // ── Confirm dialog keys ──
  if (app.confirm_open) {
    if (sym == XKB_KEY_Escape) {
      app.confirm_hover_btn = -1;
      if (app.confirm_callback) app.confirm_callback(false);
      app.confirm_open = false;
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
      app.confirm_hover_btn = -1;
      if (app.confirm_callback) app.confirm_callback(true);
      app.confirm_open = false;
      draw(app);
      return true;
    }
    return true;
  }

  // ── Settings dialog keys ──
  if (app.settings_open && app.focused_surface == app.settings_surface) {
    if (app.settings_zoom_editing) {
      if (sym == XKB_KEY_Escape) {
        app.settings_zoom_editing = false;
        app.settings_pendingRedraw = true;
        return true;
      }
      if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        double val = std::atof(app.settings_zoom_buf.c_str());
        if (val >= 50.0 && val <= 200.0) {
          apply_zoom_pct(app, val);
          app.pendingRedraw = true;
        }
        app.settings_zoom_editing = false;
        app.settings_pendingRedraw = true;
        return true;
      }
      if (sym == XKB_KEY_BackSpace) {
        if (!app.settings_zoom_buf.empty())
          app.settings_zoom_buf.pop_back();
        app.settings_pendingRedraw = true;
        return true;
      }
      if (utf8_len == 1 && utf8[0] >= '0' && utf8[0] <= '9') {
        if (app.settings_zoom_buf.size() < 3)
          app.settings_zoom_buf += utf8[0];
        app.settings_pendingRedraw = true;
        return true;
      }
      return true;
    }
    if (sym == XKB_KEY_Escape) {
      destroy_settings_window(app);
      return true;
    }
    if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && app.settings_dropdown_open) {
      if (app.settings_dropdown_hover >= 0) {
        app.settings_default_term_idx = app.settings_dropdown_hover + app.settings_dropdown_scroll;
        app.settings_dropdown_open = false;
      }
      app.settings_pendingRedraw = true;
      return true;
    }
  }

  // ── Select-by-pattern dialog keys ──
  if (app.select_pattern_open) {
    if (handle_select_pattern_key(app, sym, utf8, utf8_len)) {
      draw(app);
      return true;
    }
  }

  // ── Compress dialog keys ──
  if (app.compress_dialog_open) {
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
      app.compress_hover_btn = -1;
      execute_compress_async(app);
      return true;
    }
    if (sym == XKB_KEY_Escape) {
      app.compress_dialog_open = false;
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_BackSpace && !app.compress_name_buf.empty()) {
      app.compress_name_buf.pop_back();
      app.compress_name_cursor = static_cast<int>(app.compress_name_buf.size());
      draw(app);
      return true;
    }
    if (utf8 && utf8_len > 0 && utf8_len <= 4) {
      app.compress_name_buf.append(utf8, utf8_len);
      app.compress_name_cursor = static_cast<int>(app.compress_name_buf.size());
      draw(app);
      return true;
    }
    return false;
  }

  if (app.create_dialog_open) {
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
      if (!app.create_buf.empty()) {
        fs::path dir(app.cur_tab().current_path);
        fs::path new_path = dir / app.create_buf;
        std::error_code ec;
        bool ok = false;
        if (!app.create_template_src.empty()) {
          // Templates never overwrite — uniquify like Duplicate does.
          std::error_code eq;
          int n = 2;
          while (fs::exists(new_path, eq))
            new_path = dir / (new_path.stem().string() + " (" +
                              std::to_string(n++) + ")" +
                              new_path.extension().string());
          ok = fs::copy_file(app.create_template_src, new_path,
                             fs::copy_options::none, ec);
        } else if (app.create_is_folder) {
          ok = fs::create_directory(new_path, ec);
        } else {
          FILE* f = std::fopen(new_path.c_str(), "w");
          if (f) { ok = true; std::fclose(f); }
        }
        if (ok) {
          AppState::UndoRecord rec{app.create_is_folder
            ? AppState::UndoRecord::Type::NewFolder
            : AppState::UndoRecord::Type::NewFile, {}, {}};
          rec.paths_b.push_back(new_path.string());
          app.redo_stack.clear();
          app.undo_stack.push_back(std::move(rec));
          if (app.undo_stack.size() > app.kMaxUndo)
            app.undo_stack.erase(app.undo_stack.begin());
        }
        reload_dir(app);
      }
      app.create_dialog_open = false;
      app.create_template_src.clear();
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Escape) {
      app.create_dialog_open = false;
      app.create_template_src.clear();
      draw(app);
      return true;
    }
    // Ctrl+A select all
    if (ctrl && (sym == XKB_KEY_A || sym == XKB_KEY_a)) {
      app.create_sel_start = 0;
      app.create_sel_end = static_cast<int>(app.create_buf.size());
      app.create_cursor_pos = app.create_sel_end;
      draw(app);
      return true;
    }
    // Ctrl+C copy
    if (ctrl && (sym == XKB_KEY_C || sym == XKB_KEY_c)) {
      if (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end) {
        int a = std::min(app.create_sel_start, app.create_sel_end);
        int b = std::max(app.create_sel_start, app.create_sel_end);
        std::string sel = app.create_buf.substr(a, b - a);
        if (!sel.empty()) app.clipboard.copy_text(sel);
      }
      draw(app);
      return true;
    }
    // Ctrl+X cut
    if (ctrl && (sym == XKB_KEY_X || sym == XKB_KEY_x)) {
      if (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end) {
        int a = std::min(app.create_sel_start, app.create_sel_end);
        int b = std::max(app.create_sel_start, app.create_sel_end);
        std::string sel = app.create_buf.substr(a, b - a);
        if (!sel.empty()) app.clipboard.copy_text(sel);
        app.create_buf.erase(a, b - a);
        app.create_cursor_pos = a;
        app.create_sel_start = -1;
        app.create_sel_end = -1;
        draw(app);
      }
      return true;
    }
    // Ctrl+V paste
    if (ctrl && (sym == XKB_KEY_V || sym == XKB_KEY_v)) {
      std::string clip = app.clipboard.read_selection_text(app.wl.display());
      if (!clip.empty()) {
        if (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end) {
          int a = std::min(app.create_sel_start, app.create_sel_end);
          int b = std::max(app.create_sel_start, app.create_sel_end);
          app.create_buf.erase(a, b - a);
          app.create_cursor_pos = a;
        }
        app.create_buf.insert(app.create_cursor_pos, clip);
        app.create_cursor_pos += static_cast<int>(clip.size());
        app.create_sel_start = -1;
        app.create_sel_end = -1;
        draw(app);
      }
      return true;
    }
    if (sym == XKB_KEY_BackSpace) {
      if (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end) {
        int a = std::min(app.create_sel_start, app.create_sel_end);
        int b = std::max(app.create_sel_start, app.create_sel_end);
        app.create_buf.erase(a, b - a);
        app.create_cursor_pos = a;
        app.create_sel_start = -1;
        app.create_sel_end = -1;
      } else if (app.create_cursor_pos > 0) {
        app.create_buf.erase(app.create_cursor_pos - 1, 1);
        --app.create_cursor_pos;
      }
      { auto _n = std::chrono::steady_clock::now(); app.key_repeat_sym = sym;
        app.key_repeat_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_n.time_since_epoch()).count();
        app.key_repeat_last_ms = app.key_repeat_start_ms; }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Delete) {
      if (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end) {
        int a = std::min(app.create_sel_start, app.create_sel_end);
        int b = std::max(app.create_sel_start, app.create_sel_end);
        app.create_buf.erase(a, b - a);
        app.create_cursor_pos = a;
        app.create_sel_start = -1;
        app.create_sel_end = -1;
      } else if (app.create_cursor_pos < static_cast<int>(app.create_buf.size())) {
        app.create_buf.erase(app.create_cursor_pos, 1);
      }
      draw(app);
      return true;
    }
    // Shift+arrow selection
    if (sym == XKB_KEY_Left) {
      if (app.create_cursor_pos > 0) {
        if (shift) {
          if (app.create_sel_start < 0) app.create_sel_start = app.create_cursor_pos;
          --app.create_cursor_pos;
          app.create_sel_end = app.create_cursor_pos;
        } else {
          --app.create_cursor_pos;
          app.create_sel_start = -1;
          app.create_sel_end = -1;
        }
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Right) {
      if (app.create_cursor_pos < static_cast<int>(app.create_buf.size())) {
        if (shift) {
          if (app.create_sel_start < 0) app.create_sel_start = app.create_cursor_pos;
          ++app.create_cursor_pos;
          app.create_sel_end = app.create_cursor_pos;
        } else {
          ++app.create_cursor_pos;
          app.create_sel_start = -1;
          app.create_sel_end = -1;
        }
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Home) {
      if (shift) {
        if (app.create_sel_start < 0) app.create_sel_start = app.create_cursor_pos;
        app.create_cursor_pos = 0;
        app.create_sel_end = 0;
      } else {
        app.create_cursor_pos = 0;
        app.create_sel_start = -1;
        app.create_sel_end = -1;
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_End) {
      int end = static_cast<int>(app.create_buf.size());
      if (shift) {
        if (app.create_sel_start < 0) app.create_sel_start = app.create_cursor_pos;
        app.create_cursor_pos = end;
        app.create_sel_end = end;
      } else {
        app.create_cursor_pos = end;
        app.create_sel_start = -1;
        app.create_sel_end = -1;
      }
      draw(app);
      return true;
    }
    if (utf8 && utf8_len > 0 && utf8_len <= 4) {
      if (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end) {
        int a = std::min(app.create_sel_start, app.create_sel_end);
        int b = std::max(app.create_sel_start, app.create_sel_end);
        app.create_buf.erase(a, b - a);
        app.create_cursor_pos = a;
        app.create_sel_start = -1;
        app.create_sel_end = -1;
      }
      app.create_buf.insert(app.create_cursor_pos, utf8, utf8_len);
      app.create_cursor_pos += utf8_len;
      draw(app);
      return true;
    }
    return false;
  }

  if (app.password_dialog_open) {
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
      std::string arc = std::move(app.password_archive_path);
      std::string dst = std::move(app.password_dest_dir);
      std::string pw = std::move(app.password_buf);
      app.password_dialog_open = false;
      draw(app);
      execute_extract_with_password(app, arc, dst, pw);
      return true;
    }
    if (sym == XKB_KEY_Escape) {
      app.password_dialog_open = false;
      draw(app);
      return true;
    }
    // Ctrl+A select all
    if (ctrl && (sym == XKB_KEY_A || sym == XKB_KEY_a)) {
      app.password_sel_start = 0;
      app.password_sel_end = static_cast<int>(app.password_buf.size());
      app.password_cursor_pos = app.password_sel_end;
      draw(app);
      return true;
    }
    // Ctrl+C copy
    if (ctrl && (sym == XKB_KEY_C || sym == XKB_KEY_c)) {
      if (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end) {
        int a = std::min(app.password_sel_start, app.password_sel_end);
        int b = std::max(app.password_sel_start, app.password_sel_end);
        std::string sel = app.password_buf.substr(a, b - a);
        if (!sel.empty()) app.clipboard.copy_text(sel);
      }
      draw(app);
      return true;
    }
    // Ctrl+X cut
    if (ctrl && (sym == XKB_KEY_X || sym == XKB_KEY_x)) {
      if (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end) {
        int a = std::min(app.password_sel_start, app.password_sel_end);
        int b = std::max(app.password_sel_start, app.password_sel_end);
        std::string sel = app.password_buf.substr(a, b - a);
        if (!sel.empty()) app.clipboard.copy_text(sel);
        app.password_buf.erase(a, b - a);
        app.password_cursor_pos = a;
        app.password_sel_start = -1;
        app.password_sel_end = -1;
        draw(app);
      }
      return true;
    }
    if (ctrl && (sym == XKB_KEY_V || sym == XKB_KEY_v)) {
      std::string clip = app.clipboard.read_selection_text(app.wl.display());
      if (!clip.empty()) {
        if (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end) {
          int a = std::min(app.password_sel_start, app.password_sel_end);
          int b = std::max(app.password_sel_start, app.password_sel_end);
          app.password_buf.erase(a, b - a);
          app.password_cursor_pos = a;
        }
        app.password_buf.insert(app.password_cursor_pos, clip);
        app.password_cursor_pos += static_cast<int>(clip.size());
        app.password_sel_start = -1;
        app.password_sel_end = -1;
        draw(app);
      }
      return true;
    }
    if (sym == XKB_KEY_Left) {
      if (app.password_cursor_pos > 0) {
        if (shift) {
          if (app.password_sel_start < 0) app.password_sel_start = app.password_cursor_pos;
          --app.password_cursor_pos;
          app.password_sel_end = app.password_cursor_pos;
        } else {
          --app.password_cursor_pos;
          app.password_sel_start = -1;
          app.password_sel_end = -1;
        }
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Right) {
      if (app.password_cursor_pos < static_cast<int>(app.password_buf.size())) {
        if (shift) {
          if (app.password_sel_start < 0) app.password_sel_start = app.password_cursor_pos;
          ++app.password_cursor_pos;
          app.password_sel_end = app.password_cursor_pos;
        } else {
          ++app.password_cursor_pos;
          app.password_sel_start = -1;
          app.password_sel_end = -1;
        }
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Home) {
      if (shift) {
        if (app.password_sel_start < 0) app.password_sel_start = app.password_cursor_pos;
        app.password_cursor_pos = 0;
        app.password_sel_end = 0;
      } else {
        app.password_cursor_pos = 0;
        app.password_sel_start = -1;
        app.password_sel_end = -1;
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_End || (ctrl && (sym == XKB_KEY_E || sym == XKB_KEY_e))) {
      int end = static_cast<int>(app.password_buf.size());
      if (shift) {
        if (app.password_sel_start < 0) app.password_sel_start = app.password_cursor_pos;
        app.password_cursor_pos = end;
        app.password_sel_end = end;
      } else {
        app.password_cursor_pos = end;
        app.password_sel_start = -1;
        app.password_sel_end = -1;
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Delete) {
      if (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end) {
        int a = std::min(app.password_sel_start, app.password_sel_end);
        int b = std::max(app.password_sel_start, app.password_sel_end);
        app.password_buf.erase(a, b - a);
        app.password_cursor_pos = a;
        app.password_sel_start = -1;
        app.password_sel_end = -1;
      } else if (app.password_cursor_pos < static_cast<int>(app.password_buf.size())) {
        app.password_buf.erase(app.password_cursor_pos, 1);
        draw(app);
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_BackSpace) {
      if (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end) {
        int a = std::min(app.password_sel_start, app.password_sel_end);
        int b = std::max(app.password_sel_start, app.password_sel_end);
        app.password_buf.erase(a, b - a);
        app.password_cursor_pos = a;
        app.password_sel_start = -1;
        app.password_sel_end = -1;
      } else if (app.password_cursor_pos > 0) {
        app.password_buf.erase(app.password_cursor_pos - 1, 1);
        --app.password_cursor_pos;
        { auto _n = std::chrono::steady_clock::now(); app.key_repeat_sym = sym;
          app.key_repeat_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_n.time_since_epoch()).count();
          app.key_repeat_last_ms = app.key_repeat_start_ms; }
      }
      draw(app);
      return true;
    }
    if (utf8 && utf8_len > 0 && utf8_len <= 4) {
      if (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end) {
        int a = std::min(app.password_sel_start, app.password_sel_end);
        int b = std::max(app.password_sel_start, app.password_sel_end);
        app.password_buf.erase(a, b - a);
        app.password_cursor_pos = a;
        app.password_sel_start = -1;
        app.password_sel_end = -1;
      }
      app.password_buf.insert(app.password_cursor_pos, utf8, utf8_len);
      app.password_cursor_pos += utf8_len;
      draw(app);
      return true;
    }
    return false;
  }

  if (app.rename_ui_open) {
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
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
      return true;
    }
    if (sym == XKB_KEY_Escape) {
      app.rename_ui_open = false;
      draw(app);
      return true;
    }
    // Ctrl+A select all
    if (ctrl && (sym == XKB_KEY_A || sym == XKB_KEY_a)) {
      app.rename_ui_sel_start = 0;
      app.rename_ui_sel_end = static_cast<int>(app.rename_ui_buf.size());
      app.rename_ui_cursor_pos = app.rename_ui_sel_end;
      draw(app);
      return true;
    }
    // Ctrl+C copy
    if (ctrl && (sym == XKB_KEY_C || sym == XKB_KEY_c)) {
      if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
        int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
        int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
        std::string sel = app.rename_ui_buf.substr(a, b - a);
        if (!sel.empty()) app.clipboard.copy_text(sel);
      }
      draw(app);
      return true;
    }
    // Ctrl+X cut
    if (ctrl && (sym == XKB_KEY_X || sym == XKB_KEY_x)) {
      if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
        int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
        int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
        std::string sel = app.rename_ui_buf.substr(a, b - a);
        if (!sel.empty()) app.clipboard.copy_text(sel);
        app.rename_ui_buf.erase(a, b - a);
        app.rename_ui_cursor_pos = a;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
        draw(app);
      }
      return true;
    }
    // Ctrl+V paste
    if (ctrl && (sym == XKB_KEY_V || sym == XKB_KEY_v)) {
      std::string clip = app.clipboard.read_selection_text(app.wl.display());
      if (!clip.empty()) {
        if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
          int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
          int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
          app.rename_ui_buf.erase(a, b - a);
          app.rename_ui_cursor_pos = a;
        }
        app.rename_ui_buf.insert(app.rename_ui_cursor_pos, clip);
        app.rename_ui_cursor_pos += static_cast<int>(clip.size());
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
        draw(app);
      }
      return true;
    }
    if (sym == XKB_KEY_BackSpace && !app.rename_ui_buf.empty()) {
      if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
        int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
        int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
        app.rename_ui_buf.erase(a, b - a);
        app.rename_ui_cursor_pos = a;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      } else if (app.rename_ui_cursor_pos > 0) {
        app.rename_ui_buf.erase(app.rename_ui_cursor_pos - 1, 1);
        --app.rename_ui_cursor_pos;
      }
      if (app.rename_ui_buf.empty())
        app.key_repeat_sym = 0;
      else
        { auto _n = std::chrono::steady_clock::now(); app.key_repeat_sym = sym;
          app.key_repeat_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_n.time_since_epoch()).count();
          app.key_repeat_last_ms = app.key_repeat_start_ms; }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Delete && !app.rename_ui_buf.empty()) {
      if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
        int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
        int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
        app.rename_ui_buf.erase(a, b - a);
        app.rename_ui_cursor_pos = a;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      } else if (app.rename_ui_cursor_pos < static_cast<int>(app.rename_ui_buf.size()))
        app.rename_ui_buf.erase(app.rename_ui_cursor_pos, 1);
      else
        app.rename_ui_buf.pop_back();
      if (app.rename_ui_cursor_pos > static_cast<int>(app.rename_ui_buf.size()))
        app.rename_ui_cursor_pos = static_cast<int>(app.rename_ui_buf.size());
      { auto _n = std::chrono::steady_clock::now(); app.key_repeat_sym = sym;
        app.key_repeat_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_n.time_since_epoch()).count();
        app.key_repeat_last_ms = app.key_repeat_start_ms; }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Left && app.rename_ui_cursor_pos > 0) {
      if (shift) {
        if (app.rename_ui_sel_start < 0) app.rename_ui_sel_start = app.rename_ui_cursor_pos;
        --app.rename_ui_cursor_pos;
        app.rename_ui_sel_end = app.rename_ui_cursor_pos;
      } else {
        --app.rename_ui_cursor_pos;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      }
      { auto _n = std::chrono::steady_clock::now(); app.key_repeat_sym = sym;
        app.key_repeat_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_n.time_since_epoch()).count();
        app.key_repeat_last_ms = app.key_repeat_start_ms; }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Right && app.rename_ui_cursor_pos < static_cast<int>(app.rename_ui_buf.size())) {
      if (shift) {
        if (app.rename_ui_sel_start < 0) app.rename_ui_sel_start = app.rename_ui_cursor_pos;
        ++app.rename_ui_cursor_pos;
        app.rename_ui_sel_end = app.rename_ui_cursor_pos;
      } else {
        ++app.rename_ui_cursor_pos;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      }
      { auto _n = std::chrono::steady_clock::now(); app.key_repeat_sym = sym;
        app.key_repeat_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_n.time_since_epoch()).count();
        app.key_repeat_last_ms = app.key_repeat_start_ms; }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Home) {
      if (shift) {
        if (app.rename_ui_sel_start < 0) app.rename_ui_sel_start = app.rename_ui_cursor_pos;
        app.rename_ui_cursor_pos = 0;
        app.rename_ui_sel_end = 0;
      } else {
        app.rename_ui_cursor_pos = 0;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_End) {
      int end = static_cast<int>(app.rename_ui_buf.size());
      if (shift) {
        if (app.rename_ui_sel_start < 0) app.rename_ui_sel_start = app.rename_ui_cursor_pos;
        app.rename_ui_cursor_pos = end;
        app.rename_ui_sel_end = end;
      } else {
        app.rename_ui_cursor_pos = end;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      }
      draw(app);
      return true;
    }
    if (utf8 && utf8_len > 0 && utf8_len <= 4) {
      if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
        int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
        int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
        app.rename_ui_buf.erase(a, b - a);
        app.rename_ui_cursor_pos = a;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      }
      app.rename_ui_buf.insert(app.rename_ui_cursor_pos, utf8, utf8_len);
      app.rename_ui_cursor_pos += utf8_len;
      draw(app);
      return true;
    }
    return false;
  }

  if (app.batch_rename_open) {
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
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
      return true;
    }
    if (sym == XKB_KEY_Escape) {
      app.batch_rename_open = false;
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
      if (app.batch_rename_mode == 1) {
        // Find/replace mode: swap between find and replace
        app.batch_rename_edit_focus = (app.batch_rename_edit_focus == 0) ? 1 : 0;
        if (app.batch_rename_edit_focus == 0)
          app.batch_rename_find_cursor = static_cast<int>(app.batch_rename_find.size());
        else
          app.batch_rename_replace_cursor = static_cast<int>(app.batch_rename_replace.size());
        draw(app);
      }
      return true;
    }
    if (sym == XKB_KEY_BackSpace) {
      if (app.batch_rename_mode == 0) {
        // Template mode
        if (!app.batch_rename_template.empty()) {
          app.batch_rename_template.pop_back();
          app.batch_rename_template_cursor = static_cast<int>(app.batch_rename_template.size());
          draw(app);
        }
      } else {
        // Find/replace mode
        if (app.batch_rename_edit_focus == 0 && !app.batch_rename_find.empty()) {
          app.batch_rename_find.pop_back();
          app.batch_rename_find_cursor = static_cast<int>(app.batch_rename_find.size());
          draw(app);
          return true;
        }
        if (app.batch_rename_edit_focus == 1 && !app.batch_rename_replace.empty()) {
          app.batch_rename_replace.pop_back();
          app.batch_rename_replace_cursor = static_cast<int>(app.batch_rename_replace.size());
          draw(app);
          return true;
        }
      }
      return true;
    }
    // Text input for batch rename
    if (utf8 && utf8_len > 0 && utf8_len <= 4) {
      if (app.batch_rename_mode == 0) {
        app.batch_rename_template.append(utf8, utf8_len);
        app.batch_rename_template_cursor = static_cast<int>(app.batch_rename_template.size());
      } else {
        if (app.batch_rename_edit_focus == 0) {
          app.batch_rename_find.append(utf8, utf8_len);
          app.batch_rename_find_cursor = static_cast<int>(app.batch_rename_find.size());
        } else {
          app.batch_rename_replace.append(utf8, utf8_len);
          app.batch_rename_replace_cursor = static_cast<int>(app.batch_rename_replace.size());
        }
      }
      draw(app);
      return true;
    }
    return false;
  }

  if (app.term_chooser_open) {
    if (sym == XKB_KEY_Escape) {
      app.term_chooser_open = false;
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Up) {
      if (app.term_chooser_hover <= 0 || app.term_chooser_hover > static_cast<int>(app.term_chooser_apps.size()))
        app.term_chooser_hover = static_cast<int>(app.term_chooser_apps.size()) - 1;
      else
        app.term_chooser_hover--;
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Down) {
      if (app.term_chooser_hover < 0 || app.term_chooser_hover >= static_cast<int>(app.term_chooser_apps.size()) - 1)
        app.term_chooser_hover = 0;
      else
        app.term_chooser_hover++;
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
      if (app.term_chooser_hover >= 0 &&
          app.term_chooser_hover < static_cast<int>(app.term_chooser_apps.size())) {
        auto& chosen = app.term_chooser_apps[app.term_chooser_hover];
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
      }
      return true;
    }
    return false;
  }

  // ── Search bar keyboard handler (local + recursive) ──
  // Ctrl+H falls through so the global show-hidden toggle keeps working
  const bool show_hidden_passthrough =
      ctrl && !shift && !alt && (sym == XKB_KEY_H || sym == XKB_KEY_h);
  if (!show_hidden_passthrough &&
      ((app.active_pane ? app.r_search_active : app.search_active) ||
       (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active))) {
    auto& k_search_active = app.active_pane ? app.r_search_active : app.search_active;
    auto& k_recursive_search_active = app.active_pane ? app.r_recursive_search_active : app.recursive_search_active;
    auto& k_search_query = app.active_pane ? app.r_search_query : app.search_query;
    auto& k_recursive_search_query = app.active_pane ? app.r_recursive_search_query : app.recursive_search_query;
    auto& k_search_cursor = app.active_pane ? app.r_search_cursor : app.search_cursor;
    auto& k_search_sel_start = app.active_pane ? app.r_search_sel_start : app.search_sel_start;
    auto& k_search_sel_end = app.active_pane ? app.r_search_sel_end : app.search_sel_end;
    auto& k_search_mode = app.active_pane ? app.r_search_mode : app.search_mode;
    auto& k_search_case = app.active_pane ? app.r_search_case_sensitive : app.search_case_sensitive;
    auto trigger_search = [&] {
      bool search_from_home = k_recursive_search_active;
      (app.active_pane ? app.r_search_regex_valid : app.search_regex_valid) =
          query_is_valid(k_search_query, k_search_mode);
      k_recursive_search_query = k_search_query;
      if (!k_search_query.empty()) {
        app.cur_tab().entries.clear();
        app.cur_tab().visible_entries.clear();
        app.cur_tab().tree_entries_dirty = true;
        std::string root = search_from_home ? home_dir() : app.cur_tab().current_path;

        eh::file_browser::SearchOptions opt;
        opt.mode = k_search_mode;
        opt.case_sensitive = k_search_case;
        int ft = app.active_pane ? app.r_filter_type_idx : app.filter_type_idx;
        int fs = app.active_pane ? app.r_filter_size_idx : app.filter_size_idx;
        int fd = app.active_pane ? app.r_filter_date_idx : app.filter_date_idx;
        if (ft > 0 || fs > 0 || fd > 0) {
          opt.predicate = [ft, fs, fd](const std::string& path,
                                       const std::string& name, bool is_dir,
                                       uint64_t size, int64_t mtime) {
            return search_predicate_passes(ft, fs, fd, path, name, is_dir,
                                           size, mtime);
          };
        }
        recursive_search_worker().start_search(root, k_search_query, opt);
      } else {
        recursive_search_worker().cancel();
      }
    };

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
      reset_search_filters(app);
      bool was_recursive = k_recursive_search_active;
      k_search_active = false;
      k_recursive_search_active = false;
      k_search_query.clear();
      k_recursive_search_query.clear();
      recursive_search_worker().cancel();
      if (was_recursive)
        reload_dir(app);
      else
        reload_dir(app);
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Escape) {
      // Locked bar: first Esc only unlocks, keeping the query
      auto& k_search_locked = app.active_pane ? app.r_search_locked : app.search_locked;
      if (k_search_locked) {
        k_search_locked = false;
        draw(app);
        return true;
      }
      reset_search_filters(app);
      bool was_recursive = k_recursive_search_active;
      k_search_active = false;
      k_recursive_search_active = false;
      k_search_query.clear();
      k_recursive_search_query.clear();
      recursive_search_worker().cancel();
      if (was_recursive)
        reload_dir(app);
      else
        reload_dir(app);
      draw(app);
      return true;
    }
    if (ctrl && (sym == XKB_KEY_A || sym == XKB_KEY_a)) {
      k_search_sel_start = 0;
      k_search_sel_end = static_cast<int>(k_search_query.size());
      k_search_cursor = k_search_sel_end;
      draw(app);
      return true;
    }
    if (ctrl && (sym == XKB_KEY_C || sym == XKB_KEY_c)) {
      if (k_search_sel_start >= 0 && k_search_sel_start != k_search_sel_end) {
        int a = std::min(k_search_sel_start, k_search_sel_end);
        int b = std::max(k_search_sel_start, k_search_sel_end);
        app.clipboard.copy_text(k_search_query.substr(a, b - a));
      }
      return true;
    }
    if (sym == XKB_KEY_Left || sym == XKB_KEY_KP_Left) {
      if (k_search_cursor > 0) {
        if (shift) {
          if (k_search_sel_start < 0) k_search_sel_start = k_search_cursor;
          k_search_cursor--;
          k_search_sel_end = k_search_cursor;
        } else {
          k_search_cursor--;
          k_search_sel_start = -1;
          k_search_sel_end = -1;
        }
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Right || sym == XKB_KEY_KP_Right) {
      if (k_search_cursor < static_cast<int>(k_search_query.size())) {
        if (shift) {
          if (k_search_sel_start < 0) k_search_sel_start = k_search_cursor;
          k_search_cursor++;
          k_search_sel_end = k_search_cursor;
        } else {
          k_search_cursor++;
          k_search_sel_start = -1;
          k_search_sel_end = -1;
        }
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Home || sym == XKB_KEY_KP_Home) {
      if (shift) {
        if (k_search_sel_start < 0) k_search_sel_start = k_search_cursor;
        k_search_cursor = 0;
        k_search_sel_end = k_search_cursor;
      } else {
        k_search_cursor = 0;
        k_search_sel_start = -1;
        k_search_sel_end = -1;
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_End || sym == XKB_KEY_KP_End) {
      int end = static_cast<int>(k_search_query.size());
      if (shift) {
        if (k_search_sel_start < 0) k_search_sel_start = k_search_cursor;
        k_search_cursor = end;
        k_search_sel_end = k_search_cursor;
      } else {
        k_search_cursor = end;
        k_search_sel_start = -1;
        k_search_sel_end = -1;
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_BackSpace) {
      if (k_search_sel_start >= 0 && k_search_sel_start != k_search_sel_end) {
        int a = std::min(k_search_sel_start, k_search_sel_end);
        int b = std::max(k_search_sel_start, k_search_sel_end);
        k_search_query.erase(a, b - a);
        k_search_cursor = a;
        k_search_sel_start = -1;
        k_search_sel_end = -1;
      } else if (k_search_cursor > 0) {
        k_search_query.erase(k_search_cursor - 1, 1);
        k_search_cursor--;
      }
      trigger_search();
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Delete || sym == XKB_KEY_KP_Delete) {
      if (k_search_sel_start >= 0 && k_search_sel_start != k_search_sel_end) {
        int a = std::min(k_search_sel_start, k_search_sel_end);
        int b = std::max(k_search_sel_start, k_search_sel_end);
        k_search_query.erase(a, b - a);
        k_search_cursor = a;
        k_search_sel_start = -1;
        k_search_sel_end = -1;
      } else if (k_search_cursor < static_cast<int>(k_search_query.size())) {
        k_search_query.erase(k_search_cursor, 1);
      }
      trigger_search();
      draw(app);
      return true;
    }
    if (utf8_len > 0 && utf8[0] >= 32) {
      if (k_search_sel_start >= 0 && k_search_sel_start != k_search_sel_end) {
        int a = std::min(k_search_sel_start, k_search_sel_end);
        int b = std::max(k_search_sel_start, k_search_sel_end);
        k_search_query.erase(a, b - a);
        k_search_cursor = a;
        k_search_sel_start = -1;
        k_search_sel_end = -1;
      }
      k_search_query.insert(static_cast<std::size_t>(k_search_cursor), utf8, utf8_len);
      k_search_cursor += utf8_len;
      if (k_search_cursor > static_cast<int>(k_search_query.size()))
        k_search_cursor = static_cast<int>(k_search_query.size());
      trigger_search();
      draw(app);
      return true;
    }
    return true;
  }

  // ── Path editing keyboard handler ──
  // Ctrl+H falls through so the global show-hidden toggle keeps working
  if (!show_hidden_passthrough &&
      (app.active_pane ? app.r_path_editing : app.path_editing)) {
    auto& pe_editing = app.active_pane ? app.r_path_editing : app.path_editing;
    auto& pe_buf = app.active_pane ? app.r_path_edit_buf : app.path_edit_buf;
    auto& pe_cursor = app.active_pane ? app.r_path_edit_cursor : app.path_edit_cursor;
    auto& pe_sel_start = app.active_pane ? app.r_path_edit_sel_start : app.path_edit_sel_start;
    auto& pe_sel_end = app.active_pane ? app.r_path_edit_sel_end : app.path_edit_sel_end;
    auto& pe_dragging = app.active_pane ? app.r_path_edit_dragging : app.path_edit_dragging;
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
      pe_editing = false;
      pe_dragging = false;
      std::string new_path = pe_buf;
      if (fs::exists(new_path)) {
        navigate_to(app, new_path);
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Escape) {
      pe_editing = false;
      pe_dragging = false;
      pe_sel_start = -1;
      pe_sel_end = -1;
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_BackSpace) {
      if (pe_sel_start >= 0 && pe_sel_start != pe_sel_end) {
        int sel_a = std::min(pe_sel_start, pe_sel_end);
        int sel_b = std::max(pe_sel_start, pe_sel_end);
        pe_buf.erase(sel_a, sel_b - sel_a);
        pe_cursor = sel_a;
      } else if (pe_cursor > 0) {
        pe_buf.erase(pe_cursor - 1, 1);
        pe_cursor--;
      }
      pe_sel_start = -1;
      pe_sel_end = -1;
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Delete) {
      if (pe_sel_start >= 0 && pe_sel_start != pe_sel_end) {
        int sel_a = std::min(pe_sel_start, pe_sel_end);
        int sel_b = std::max(pe_sel_start, pe_sel_end);
        pe_buf.erase(sel_a, sel_b - sel_a);
        pe_cursor = sel_a;
      } else if (pe_cursor < static_cast<int>(pe_buf.size())) {
        pe_buf.erase(pe_cursor, 1);
      }
      pe_sel_start = -1;
      pe_sel_end = -1;
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Left) {
      if (pe_cursor > 0) {
        if (shift) {
          if (pe_sel_start < 0)
            pe_sel_start = pe_cursor;
          pe_cursor--;
          pe_sel_end = pe_cursor;
        } else {
          pe_cursor--;
          pe_sel_start = -1;
          pe_sel_end = -1;
        }
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Right) {
      if (pe_cursor < static_cast<int>(pe_buf.size())) {
        if (shift) {
          if (pe_sel_start < 0)
            pe_sel_start = pe_cursor;
          pe_cursor++;
          pe_sel_end = pe_cursor;
        } else {
          pe_cursor++;
          pe_sel_start = -1;
          pe_sel_end = -1;
        }
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Home) {
      if (shift) {
        if (pe_sel_start < 0)
          pe_sel_start = pe_cursor;
        pe_cursor = 0;
        pe_sel_end = 0;
      } else {
        pe_cursor = 0;
        pe_sel_start = -1;
        pe_sel_end = -1;
      }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_End) {
      int end_pos = static_cast<int>(pe_buf.size());
      if (shift) {
        if (pe_sel_start < 0)
          pe_sel_start = pe_cursor;
        pe_cursor = end_pos;
        pe_sel_end = end_pos;
      } else {
        pe_cursor = end_pos;
        pe_sel_start = -1;
        pe_sel_end = -1;
      }
      draw(app);
      return true;
    }
    if (ctrl && (sym == XKB_KEY_A || sym == XKB_KEY_a)) {
      pe_sel_start = 0;
      pe_sel_end = static_cast<int>(pe_buf.size());
      pe_cursor = pe_sel_end;
      draw(app);
      return true;
    }
    if (ctrl && (sym == XKB_KEY_C || sym == XKB_KEY_c)) {
      if (pe_sel_start >= 0 && pe_sel_start != pe_sel_end) {
        int sel_a = std::min(pe_sel_start, pe_sel_end);
        int sel_b = std::max(pe_sel_start, pe_sel_end);
        std::string selected = pe_buf.substr(sel_a, sel_b - sel_a);
        if (!selected.empty()) {
          app.clipboard.copy_text(selected);
        }
      }
      draw(app);
      return true;
    }
    if (utf8 && utf8_len > 0 && !ctrl && !alt) {
      if (pe_sel_start >= 0 && pe_sel_start != pe_sel_end) {
        int sel_a = std::min(pe_sel_start, pe_sel_end);
        int sel_b = std::max(pe_sel_start, pe_sel_end);
        pe_buf.erase(sel_a, sel_b - sel_a);
        pe_cursor = sel_a;
        pe_sel_start = -1;
        pe_sel_end = -1;
      }
      pe_buf.insert(static_cast<std::size_t>(pe_cursor), utf8, utf8_len);
      pe_cursor += utf8_len;
      draw(app);
      return true;
    }
    return true;
  }

  // ── Tab shortcuts ──
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

  if (app.create_dialog_open && utf8 && utf8_len > 0 && utf8_len <= 4) {
    if (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end) {
      int a = std::min(app.create_sel_start, app.create_sel_end);
      int b = std::max(app.create_sel_start, app.create_sel_end);
      app.create_buf.erase(a, b - a);
      app.create_cursor_pos = a;
      app.create_sel_start = -1;
      app.create_sel_end = -1;
    }
    app.create_buf.insert(app.create_cursor_pos, utf8, utf8_len);
    app.create_cursor_pos += utf8_len;
    draw(app);
    return true;
  }

  if (app.password_dialog_open && utf8 && utf8_len > 0 && utf8_len <= 4) {
    if (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end) {
      int a = std::min(app.password_sel_start, app.password_sel_end);
      int b = std::max(app.password_sel_start, app.password_sel_end);
      app.password_buf.erase(a, b - a);
      app.password_cursor_pos = a;
      app.password_sel_start = -1;
      app.password_sel_end = -1;
    }
    app.password_buf.insert(app.password_cursor_pos, utf8, utf8_len);
    app.password_cursor_pos += utf8_len;
    draw(app);
    return true;
  }

  if (app.rename_ui_open && utf8 && utf8_len > 0 && utf8_len <= 4) {
    if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
      int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
      int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
      app.rename_ui_buf.erase(a, b - a);
      app.rename_ui_cursor_pos = a;
      app.rename_ui_sel_start = -1;
      app.rename_ui_sel_end = -1;
    }
    app.rename_ui_buf.insert(app.rename_ui_cursor_pos, utf8, utf8_len);
    app.rename_ui_cursor_pos += utf8_len;
    draw(app);
    return true;
  }

  // ── Type-to-find: printable character activates search bar ──
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

  // ── Space preview dismiss ──
  if (app.preview_mode == AppState::PreviewMode::Space && sym == XKB_KEY_Escape) {
    reset_preview(app);
    draw(app);
    return true;
  }

  // ── Dismiss drop action chooser ──
  if (app.drop_chooser_open && sym == XKB_KEY_Escape) {
    app.drop_chooser_open = false;
    app.drop_chooser_hover = -1;
    app.drop_chooser_srcs.clear();
    app.drop_chooser_target.clear();
    draw(app);
    return true;
  }

  // ── Escape: clear cut indicator and selection ──
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
