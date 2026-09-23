// key_dialog.cpp — dialog-region key handlers (properties through
// terminal chooser, plus the printable-text fallback into the text dialogs).
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
// ── UTF-8 cursor helpers (byte offset ↔ codepoint boundary) ────────────────
// Buffers are UTF-8 std::string with cursor stored as a byte offset. Moving or
// erasing by a single byte splits multi-byte codepoints and leaves invalid
// UTF-8 behind (renders as a tofu box). These step by whole codepoints.
static int utf8_prev_pos(const std::string& s, int pos) {
  if (pos <= 0) return 0;
  int n = static_cast<int>(s.size());
  if (pos > n) pos = n;
  --pos;
  while (pos > 0 &&
         (static_cast<unsigned char>(s[static_cast<std::size_t>(pos)]) & 0xC0) == 0x80)
    --pos;
  return pos;
}

static int utf8_next_pos(const std::string& s, int pos) {
  if (pos < 0) return 0;
  int n = static_cast<int>(s.size());
  if (pos >= n) return n;
  ++pos;
  while (pos < n &&
         (static_cast<unsigned char>(s[static_cast<std::size_t>(pos)]) & 0xC0) == 0x80)
    ++pos;
  return pos;
}

static int word_prev_pos(const std::string& s, int pos) {
  int p = pos;
  int n = static_cast<int>(s.size());
  if (p > n) p = n;
  // Skip spaces/tabs backwards, then the word itself.
  while (p > 0 && (s[static_cast<std::size_t>(p - 1)] == ' ' ||
                   s[static_cast<std::size_t>(p - 1)] == '\t'))
    p = utf8_prev_pos(s, p);
  while (p > 0 && s[static_cast<std::size_t>(p - 1)] != ' ' &&
         s[static_cast<std::size_t>(p - 1)] != '\t')
    p = utf8_prev_pos(s, p);
  return p;
}

static int word_next_pos(const std::string& s, int pos) {
  int p = pos;
  int n = static_cast<int>(s.size());
  if (p < 0) p = 0;
  if (p > n) p = n;
  while (p < n && (s[static_cast<std::size_t>(p)] == ' ' ||
                  s[static_cast<std::size_t>(p)] == '\t'))
    p = utf8_next_pos(s, p);
  while (p < n && s[static_cast<std::size_t>(p)] != ' ' &&
         s[static_cast<std::size_t>(p)] != '\t')
    p = utf8_next_pos(s, p);
  return p;
}

static void clamp_cursor(const std::string& s, int& cursor) {
  int n = static_cast<int>(s.size());
  if (cursor < 0) cursor = 0;
  if (cursor > n) cursor = n;
  // Snap inside-codepoint positions back to a boundary so substr() stays
  // valid UTF-8 (prevents tofu boxes in cairo).
  while (cursor > 0 && cursor < n &&
         (static_cast<unsigned char>(s[static_cast<std::size_t>(cursor)]) & 0xC0) == 0x80)
    --cursor;
}
// ── key region handlers, in original flow order ─────────────────────────────

bool key_properties(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
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
  return false;
}


bool key_confirm(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
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
  return false;
}


bool key_settings(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
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
  return false;
}


bool key_select_pattern(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
  if (app.select_pattern_open) {
    if (handle_select_pattern_key(app, sym, utf8, utf8_len)) {
      draw(app);
      return true;
    }
  }
  return false;
}


bool key_compress(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
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
  return false;
}


bool key_create(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
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
  return false;
}


bool key_password(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
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
  return false;
}


bool key_rename_ui(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
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
    if (sym == XKB_KEY_BackSpace) {
      // Always consume while the dialog is open so a press on an empty buffer
      // or at column 0 never leaks to navigate_up() behind the dialog.
      if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
        int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
        int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
        clamp_cursor(app.rename_ui_buf, a);
        clamp_cursor(app.rename_ui_buf, b);
        app.rename_ui_buf.erase(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
        app.rename_ui_cursor_pos = a;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      } else if (app.rename_ui_cursor_pos > 0) {
        clamp_cursor(app.rename_ui_buf, app.rename_ui_cursor_pos);
        if (ctrl) {
          int p = word_prev_pos(app.rename_ui_buf, app.rename_ui_cursor_pos);
          app.rename_ui_buf.erase(static_cast<std::size_t>(p),
                                  static_cast<std::size_t>(app.rename_ui_cursor_pos - p));
          app.rename_ui_cursor_pos = p;
        } else {
          int p = utf8_prev_pos(app.rename_ui_buf, app.rename_ui_cursor_pos);
          app.rename_ui_buf.erase(static_cast<std::size_t>(p),
                                  static_cast<std::size_t>(app.rename_ui_cursor_pos - p));
          app.rename_ui_cursor_pos = p;
        }
      }
      clamp_cursor(app.rename_ui_buf, app.rename_ui_cursor_pos);
      if (app.rename_ui_buf.empty() || app.rename_ui_cursor_pos <= 0)
        app.key_repeat_sym = 0;
      else
        { auto _n = std::chrono::steady_clock::now(); app.key_repeat_sym = sym;
          app.key_repeat_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_n.time_since_epoch()).count();
          app.key_repeat_last_ms = app.key_repeat_start_ms; }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Delete) {
      // Always consume; at end-of-text Delete is a no-op (never pop_back).
      bool erased = false;
      if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
        int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
        int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
        clamp_cursor(app.rename_ui_buf, a);
        clamp_cursor(app.rename_ui_buf, b);
        app.rename_ui_buf.erase(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
        app.rename_ui_cursor_pos = a;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
        erased = true;
      } else if (app.rename_ui_cursor_pos < static_cast<int>(app.rename_ui_buf.size())) {
        clamp_cursor(app.rename_ui_buf, app.rename_ui_cursor_pos);
        if (ctrl) {
          int p = word_next_pos(app.rename_ui_buf, app.rename_ui_cursor_pos);
          app.rename_ui_buf.erase(static_cast<std::size_t>(app.rename_ui_cursor_pos),
                                  static_cast<std::size_t>(p - app.rename_ui_cursor_pos));
        } else {
          int p = utf8_next_pos(app.rename_ui_buf, app.rename_ui_cursor_pos);
          app.rename_ui_buf.erase(static_cast<std::size_t>(app.rename_ui_cursor_pos),
                                  static_cast<std::size_t>(p - app.rename_ui_cursor_pos));
        }
        erased = true;
      }
      clamp_cursor(app.rename_ui_buf, app.rename_ui_cursor_pos);
      if (app.rename_ui_buf.empty() || !erased)
        app.key_repeat_sym = 0;
      else
        { auto _n = std::chrono::steady_clock::now(); app.key_repeat_sym = sym;
          app.key_repeat_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_n.time_since_epoch()).count();
          app.key_repeat_last_ms = app.key_repeat_start_ms; }
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Left) {
      // Always consume so the file selection behind the dialog never moves.
      clamp_cursor(app.rename_ui_buf, app.rename_ui_cursor_pos);
      bool moved = false;
      if (app.rename_ui_cursor_pos > 0) {
        int p = ctrl ? word_prev_pos(app.rename_ui_buf, app.rename_ui_cursor_pos)
                     : utf8_prev_pos(app.rename_ui_buf, app.rename_ui_cursor_pos);
        if (shift) {
          if (app.rename_ui_sel_start < 0) app.rename_ui_sel_start = app.rename_ui_cursor_pos;
          app.rename_ui_cursor_pos = p;
          app.rename_ui_sel_end = app.rename_ui_cursor_pos;
        } else {
          app.rename_ui_cursor_pos = p;
          app.rename_ui_sel_start = -1;
          app.rename_ui_sel_end = -1;
        }
        moved = true;
      } else if (!shift) {
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      }
      if (moved)
        { auto _n = std::chrono::steady_clock::now(); app.key_repeat_sym = sym;
          app.key_repeat_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_n.time_since_epoch()).count();
          app.key_repeat_last_ms = app.key_repeat_start_ms; }
      else
        app.key_repeat_sym = 0;
      draw(app);
      return true;
    }
    if (sym == XKB_KEY_Right) {
      // Always consume; a space counts as a character and the cursor steps it.
      clamp_cursor(app.rename_ui_buf, app.rename_ui_cursor_pos);
      bool moved = false;
      if (app.rename_ui_cursor_pos < static_cast<int>(app.rename_ui_buf.size())) {
        int p = ctrl ? word_next_pos(app.rename_ui_buf, app.rename_ui_cursor_pos)
                     : utf8_next_pos(app.rename_ui_buf, app.rename_ui_cursor_pos);
        if (shift) {
          if (app.rename_ui_sel_start < 0) app.rename_ui_sel_start = app.rename_ui_cursor_pos;
          app.rename_ui_cursor_pos = p;
          app.rename_ui_sel_end = app.rename_ui_cursor_pos;
        } else {
          app.rename_ui_cursor_pos = p;
          app.rename_ui_sel_start = -1;
          app.rename_ui_sel_end = -1;
        }
        moved = true;
      } else if (!shift) {
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      }
      if (moved)
        { auto _n = std::chrono::steady_clock::now(); app.key_repeat_sym = sym;
          app.key_repeat_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_n.time_since_epoch()).count();
          app.key_repeat_last_ms = app.key_repeat_start_ms; }
      else
        app.key_repeat_sym = 0;
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
    if (utf8 && utf8_len > 0 && utf8_len <= 4 && !ctrl && !alt) {
      const unsigned char c0 = static_cast<unsigned char>(utf8[0]);
      // Never insert C0 controls / DEL (Tab, etc.) into a file name. They are
      // swallowed so they can't leak to the browser or leave tofu behind.
      // Multi-byte sequences (lead byte >= 0x80) are printable by definition.
      const bool printable =
          (utf8_len > 1) || (c0 >= 0x20 && c0 != 0x7f);
      if (!printable) return true;
      if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
        int a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
        int b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
        clamp_cursor(app.rename_ui_buf, a);
        clamp_cursor(app.rename_ui_buf, b);
        app.rename_ui_buf.erase(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
        app.rename_ui_cursor_pos = a;
        app.rename_ui_sel_start = -1;
        app.rename_ui_sel_end = -1;
      }
      clamp_cursor(app.rename_ui_buf, app.rename_ui_cursor_pos);
      app.rename_ui_buf.insert(static_cast<std::size_t>(app.rename_ui_cursor_pos), utf8,
                               static_cast<std::size_t>(utf8_len));
      app.rename_ui_cursor_pos += utf8_len;
      clamp_cursor(app.rename_ui_buf, app.rename_ui_cursor_pos);
      draw(app);
      return true;
    }
    // Modal: swallow anything else (Up/Down, F-keys, Ctrl+letter shortcuts…)
    // so the browser behind the dialog never reacts while renaming.
    return true;
  }
  return false;
}


bool key_batch_rename(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
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
  return false;
}


bool key_term_chooser(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
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
  return false;
}


bool key_text_fallback(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len) {
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
  return false;
}


} // namespace eh::file_browser
