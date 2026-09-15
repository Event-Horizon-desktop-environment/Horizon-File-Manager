// key_edit.cpp — text-editing key handlers (search bar and path editing).
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

bool key_search(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                const char* utf8, int utf8_len, bool show_hidden_passthrough) {
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
  return false;
}


bool key_path_edit(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                const char* utf8, int utf8_len, bool show_hidden_passthrough) {
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
  return false;
}


} // namespace eh::file_browser
