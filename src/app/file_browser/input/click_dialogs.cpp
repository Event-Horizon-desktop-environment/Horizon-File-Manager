// click_dialogs.cpp — dialogs split from click.cpp (handle_click region carve).
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

bool click_drop_chooser(AppState& app, int x, int y, int button) {
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
    return true;
  }
  return false;
}

bool click_context_menu(AppState& app, int x, int y, int button) {
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
        return true;
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
            return true;
          }
        }
      }
      app.context_menu_open = false;
    } else {
      app.context_menu_open = false;
    }
    draw(app);
    return true;
  }
  return false;
}

bool click_properties(AppState& app, int x, int y, int button) {
  // ── Properties dialog clicks ──
  if (app.properties.open) {
    if (button == 0x110) {
      int hit = properties_hit_test(app, x, y);

      if (hit == -1 || hit == -2) {
        destroy_props_window(app);
        return true;
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
        return true;
      }

      // Combo dropdown toggle
      if (hit >= 10 && hit <= 12) {
        int pi = hit - 10;
        if (app.properties.combo_open == pi)
          app.properties.combo_open = -1;
        else
          app.properties.combo_open = pi;
        draw(app);
        return true;
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
        return true;
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
        return true;
      }

      // Clicked elsewhere inside dialog — close any open combo
      if (app.properties.combo_open >= 0) {
        app.properties.combo_open = -1;
        draw(app);
        return true;
      }
    }
  }
  return false;
}

bool click_conflict(AppState& app, int x, int y, int button) {
  // ── Overwrite/merge conflict dialog clicks ──
  if (app.conflict_open) {
    if (button == 0x110) {
      // Checkbox
      const auto& cr = app.conflict_check_rect;
      if (app.pointerX >= cr[0] && app.pointerX < cr[0] + cr[2] &&
          app.pointerY >= cr[1] && app.pointerY < cr[1] + cr[3]) {
        app.conflict_apply_all = !app.conflict_apply_all;
        draw(app);
        return true;
      }
      // Buttons: 0=Skip, 1=Cancel, 2=Overwrite/Merge
      for (int b = 0; b < 3; ++b) {
        const auto& r = app.conflict_btn_rects[b];
        if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) {
          resolve_conflict_choice(app, b);
          return true;
        }
      }
      // Modal — ignore clicks elsewhere while open
    }
    return true;
  }
  return false;
}

bool click_confirm(AppState& app, int x, int y, int button) {
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
      return true;
    }

    if (button == 0x110 && ((x >= cancel_x && x < cancel_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) ||
        (x < dlg_x || x > dlg_x + dlg_w ||
         y < dlg_y || y > dlg_y + dlg_h))) {
      app.confirm_hover_btn = -1;
      if (app.confirm_callback) app.confirm_callback(false);
      app.confirm_open = false;
      draw(app);
      return true;
    }
    return true;
  }
  return false;
}

bool click_password(AppState& app, int x, int y, int button) {
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
      return true;
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
      return true;
    }

    if (button == 0x110 && x >= extract_x && x < extract_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) {
      std::string arc = std::move(app.password_archive_path);
      std::string dst = std::move(app.password_dest_dir);
      std::string pw = std::move(app.password_buf);
      app.password_dialog_open = false;
      draw(app);
      execute_extract_with_password(app, arc, dst, pw);
      return true;
    }

    if (button == 0x110 && ((x >= btns_x && x < btns_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) ||
        (x < cx || x > cx + card_w ||
         y < cy || y > cy + card_h))) {
      app.password_dialog_open = false;
      draw(app);
      return true;
    }
    return true;
  }
  return false;
}

bool click_compress(AppState& app, int x, int y, int button) {
  // ── Compress dialog clicks ──
  if (app.compress_dialog_open) {
    int dlg_w = 420;
    int dlg_h = 372;
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
          return true;
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
          return true;
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
          return true;
        }
      }

      // Threads buttons
      const std::vector<int> thread_opts = compress_thread_options();
      int th_btn_y = lvl_btn_y + lvl_btn_h + 12 + 18;
      int th_btn_w = compress_thread_btn_w(static_cast<int>(thread_opts.size()));
      int th_btn_h = 28;
      int th_gap = 8;
      for (size_t ti = 0; ti < thread_opts.size(); ++ti) {
        int tx = content_x + static_cast<int>(ti) * (th_btn_w + th_gap);
        if (x >= tx && x < tx + th_btn_w && y >= th_btn_y && y < th_btn_y + th_btn_h) {
          app.compress_threads = thread_opts[ti];
          draw(app);
          return true;
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
        return true;
      }

      if ((x >= cancel_x && x < cancel_x + btn_w &&
           y >= btn_y && y < btn_y + btn_h) ||
          (x < dlg_x || x > dlg_x + dlg_w ||
           y < dlg_y || y > dlg_y + dlg_h)) {
        app.compress_dialog_open = false;
        draw(app);
        return true;
      }
    }
    return true;
  }
  return false;
}

bool click_select_pattern(AppState& app, int x, int y, int button) {
  // ── Select-by-pattern dialog clicks ──
  if (app.select_pattern_open) {
    if (handle_select_pattern_click(app, button, x, y)) {
      draw(app);
      return true;
    }
  }
  return false;
}

bool click_create(AppState& app, int x, int y, int button) {
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
      return true;
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
      return true;
    }

    if (button == 0x110 && x >= cancel_x && x < cancel_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) {
      app.create_dialog_open = false;
      app.create_template_src.clear();
      draw(app);
      return true;
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
      return true;
    }

    if (button == 0x110 && (x < dlg_x || x > dlg_x + dlg_w ||
        y < dlg_y || y > dlg_y + dlg_h)) {
      app.create_dialog_open = false;
      app.create_template_src.clear();
      draw(app);
      return true;
    }
    return true;
  }
  return false;
}

bool click_rename_ui(AppState& app, int x, int y, int button) {
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
      return true;
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
      return true;
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
      return true;
    }

    if (button == 0x110 && x >= cancel_x && x < cancel_x + btn_w &&
        y >= btn_y && y < btn_y + btn_h) {
      app.rename_ui_open = false;
      draw(app);
      return true;
    }

    if (button == 0x110 && (x < dlg_x || x > dlg_x + dlg_w ||
        y < dlg_y || y > dlg_y + dlg_h)) {
      app.rename_ui_open = false;
      draw(app);
      return true;
    }
    return true;
  }
  return false;
}

bool click_batch_rename(AppState& app, int x, int y, int button) {
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
          return true;
        }
        if (x >= cx + tab_w + 8 && x < cx + tab_w + 8 + tab_w) {
          if (app.batch_rename_mode != 1) {
            app.batch_rename_mode = 1;
            app.batch_rename_edit_focus = 0;
            draw(app);
          }
          return true;
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
          return true;
        }

        // ── [+ Add] button click ──
        int add_x = tf_x + tf_w + 8;
        int add_w = 70;
        if (x >= add_x && x < add_x + add_w && y >= tf_y && y < tf_y + field_h) {
          app.batch_rename_show_add = !app.batch_rename_show_add;
          app.batch_rename_add_hover = -1;
          draw(app);
          return true;
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
            return true;
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
          return true;
        }

        // Replace field
        int rl_y = input_y + field_h + 6;
        if (x >= fld_x && x < fld_x + fld_w && y >= rl_y && y < rl_y + field_h) {
          app.batch_rename_edit_focus = 1;
          app.batch_rename_replace_cursor = static_cast<int>(app.batch_rename_replace.size());
          draw(app);
          return true;
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
        return true;
      }

      // ── Cancel button or click outside ──
      if ((x >= cancel_x && x < cancel_x + btn_w && y >= btn_y && y < btn_y + btn_h) ||
          (x < dlg_x || x > dlg_x + dlg_w || y < dlg_y || y > dlg_y + dlg_h)) {
        app.batch_rename_open = false;
        draw(app);
        return true;
      }
    }
    return true;
  }
  return false;
}

bool click_open_with(AppState& app, int x, int y, int button) {
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
        return true;
      }

      // Close button
      if (dx >= app.open_with_hit_close[0] && dx < app.open_with_hit_close[0] + app.open_with_hit_close[2] &&
          dy >= app.open_with_hit_close[1] && dy < app.open_with_hit_close[1] + app.open_with_hit_close[3]) {
        open_with_close(app);
        draw(app);
        return true;
      }

      // Cancel button
      if (dx >= app.open_with_hit_cancel[0] && dx < app.open_with_hit_cancel[0] + app.open_with_hit_cancel[2] &&
          dy >= app.open_with_hit_cancel[1] && dy < app.open_with_hit_cancel[1] + app.open_with_hit_cancel[3]) {
        open_with_close(app);
        draw(app);
        return true;
      }

      // "Set as Default" toggle
      if (dx >= app.open_with_hit_default[0] && dx < app.open_with_hit_default[0] + app.open_with_hit_default[2] &&
          dy >= app.open_with_hit_default[1] && dy < app.open_with_hit_default[1] + app.open_with_hit_default[3]) {
        app.open_with_set_default = !app.open_with_set_default;
        draw(app);
        return true;
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
        return true;
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
            return true;
          }
          cy_off += entry_h;
        }
      }
    }
    return true;
  }
  return false;
}

bool click_term_chooser(AppState& app, int x, int y, int button) {
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
        return true;
      }
      if (x >= close_x && x < close_x + 28 && y >= close_y && y < close_y + 28) {
        app.term_chooser_open = false;
        draw(app);
        return true;
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
          return true;
        }
      }
    }
    return true;
  }
  return false;
}

} // namespace eh::file_browser
