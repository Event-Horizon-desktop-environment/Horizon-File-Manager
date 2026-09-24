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
  // Resolved through the retained hit registry (rects stored during paint);
  // no geometry is re-derived here.
  if (!app.conflict_open) return false;
  if (button != 0x110) return true; // modal: swallow other buttons
  using hui::Hit::dialog;
  using hui::Hit::kConflictBtnBase;
  using hui::Hit::kConflictCheck;
  using hui::Hit::kDlgConflict;
  const uint32_t hid = app.hit_main.query(x, y);
  // Checkbox
  if (hid == dialog(kDlgConflict, kConflictCheck)) {
    app.conflict_apply_all = !app.conflict_apply_all;
    draw(app);
    return true;
  }
  // Buttons: 0=Skip, 1=Cancel, 2=Overwrite/Merge
  {
    int ctrl = hui::Hit::dialog_ctrl(hid);
    int b = ctrl - kConflictBtnBase;
    if (b >= 0 && b < 3 && (hid & 0xFFFFC00) == dialog(kDlgConflict, 0)) {
      resolve_conflict_choice(app, b);
      return true;
    }
  }
  // Modal — ignore clicks elsewhere while open
  return true;
}

bool click_confirm(AppState& app, int x, int y, int button) {
  // ── Confirm dialog clicks ──
  // Resolved through the retained hit registry (rects stored during paint
  // from hui layout nodes); no geometry is re-derived here.
  if (!app.confirm_open) return false;
  using hui::Hit::dialog;
  using hui::Hit::kConfirmCancel;
  using hui::Hit::kConfirmDelete;
  using hui::Hit::kDlgConfirm;
  const uint32_t hid = app.hit_main.query(x, y);

  if (button == 0x110 && hid == dialog(kDlgConfirm, kConfirmDelete)) {
    app.confirm_hover_btn = -1;
    if (app.confirm_callback) app.confirm_callback(true);
    app.confirm_open = false;
    draw(app);
    return true;
  }

  if (button == 0x110 && (hid == dialog(kDlgConfirm, kConfirmCancel) ||
                          hid != dialog(kDlgConfirm, 0))) {
    app.confirm_hover_btn = -1;
    if (app.confirm_callback) app.confirm_callback(false);
    app.confirm_open = false;
    draw(app);
    return true;
  }
  return true;
}

bool click_password(AppState& app, int x, int y, int button) {
  // ── Password dialog clicks ──
  // Resolved through the retained hit registry (rects stored during paint
  // from hui layout nodes); no geometry is re-derived here.
  if (!app.password_dialog_open) return false;
  using hui::Hit::dialog;
  using hui::Hit::kDlgPassword;
  using hui::Hit::kPasswordCancel;
  using hui::Hit::kPasswordExtract;
  using hui::Hit::kPasswordInput;
  const uint32_t hid = app.hit_main.query(x, y);

    // Right-click on input field → text context menu
    if (button == 0x111 && hid == dialog(kDlgPassword, kPasswordInput)) {
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
    if (button == 0x110 && hid == dialog(kDlgPassword, kPasswordInput)) {
      const hui::HitRegion* region = app.hit_main.find(x, y);
      int origin_x = region ? region->x : 0;
      std::string masked(app.password_buf.size(), '*');
      cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t* cr_tmp = cairo_create(tmp);
      cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr_tmp, 14);
      int click_x = x - origin_x - 14;
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

    if (button == 0x110 && hid == dialog(kDlgPassword, kPasswordExtract)) {
      std::string arc = std::move(app.password_archive_path);
      std::string dst = std::move(app.password_dest_dir);
      std::string pw = std::move(app.password_buf);
      app.password_dialog_open = false;
      draw(app);
      execute_extract_with_password(app, arc, dst, pw);
      return true;
    }

    if (button == 0x110 && (hid == dialog(kDlgPassword, kPasswordCancel) ||
                            hid != dialog(kDlgPassword, 0))) {
      app.password_dialog_open = false;
      draw(app);
      return true;
    }
    return true;
}

bool click_compress(AppState& app, int x, int y, int button) {
  // ── Compress dialog clicks ──
  // Resolved through the retained hit registry (rects stored during paint
  // from hui layout nodes); no geometry is re-derived here.
  if (!app.compress_dialog_open) return false;
  if (button != 0x110) return true; // modal: swallow other buttons
  using hui::Hit::dialog;
  using hui::Hit::kCompressCancel;
  using hui::Hit::kCompressFormatBase;
  using hui::Hit::kCompressLevelBase;
  using hui::Hit::kCompressOk;
  using hui::Hit::kCompressThreadBase;
  using hui::Hit::kDlgCompress;
  const uint32_t hid = app.hit_main.query(x, y);
  if ((hid & hui::Hit::kGroupMask) != hui::Hit::kDialog ||
      ((hid & 0xFFFFC00) != dialog(kDlgCompress, 0)))
    return true; // outside this dialog's regions: swallow (modal)

  const int ctrl = hui::Hit::dialog_ctrl(hid);
  static constexpr int kLevelValues[5] = {0, 3, 6, 8, 9};

  // Format chips
  if (ctrl >= kCompressFormatBase && ctrl < kCompressFormatBase + 7) {
    int i = ctrl - kCompressFormatBase;
    if (i >= 0 && i < 7 && app.compress_format_available[i]) {
      app.compress_format = i;
      draw(app);
    }
    return true;
  }

  // Level chips
  if (ctrl >= kCompressLevelBase && ctrl < kCompressLevelBase + 5) {
    app.compress_level = kLevelValues[ctrl - kCompressLevelBase];
    draw(app);
    return true;
  }

  // Thread chips
  if (ctrl >= kCompressThreadBase) {
    const std::vector<int> thread_opts = compress_thread_options();
    int ti = ctrl - kCompressThreadBase;
    if (ti >= 0 && ti < static_cast<int>(thread_opts.size())) {
      app.compress_threads = thread_opts[static_cast<size_t>(ti)];
      draw(app);
    }
    return true;
  }

  // Bottom buttons
  if (hid == dialog(kDlgCompress, kCompressOk)) {
    app.compress_hover_btn = -1;
    execute_compress_async(app);
    return true;
  }

  if (hid == dialog(kDlgCompress, kCompressCancel)) {
    app.compress_dialog_open = false;
    draw(app);
    return true;
  }
  return true;
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
  // Resolved through the retained hit registry (rects stored during paint
  // from hui layout nodes); no geometry is re-derived here.
  if (!app.create_dialog_open) return false;
  using hui::Hit::dialog;
  using hui::Hit::kCreateCancel;
  using hui::Hit::kCreateInput;
  using hui::Hit::kCreateOk;
  using hui::Hit::kDlgCreate;
  const uint32_t hid = app.hit_main.query(x, y);

  // Right-click on input field → text context menu
  if (button == 0x111 && hid == dialog(kDlgCreate, kCreateInput)) {
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

  if (button != 0x110) return true; // modal: swallow other buttons

  if (hid == dialog(kDlgCreate, kCreateOk)) {
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

  if (hid == dialog(kDlgCreate, kCreateCancel)) {
    app.create_dialog_open = false;
    app.create_template_src.clear();
    draw(app);
    return true;
  }

  // Left-click on input field → position cursor + start drag
  if (hid == dialog(kDlgCreate, kCreateInput)) {
    const hui::HitRegion* region = app.hit_main.find(x, y);
    int origin_x = region ? region->x : 0;
    cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr_tmp = cairo_create(tmp);
    cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr_tmp, 14);
    int click_x = x - origin_x - 10;
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

  if (hid != dialog(kDlgCreate, 0)) {
    app.create_dialog_open = false;
    app.create_template_src.clear();
    draw(app);
  }
  return true;
}

bool click_rename_ui(AppState& app, int x, int y, int button) {
  // ── Rename UI dialog clicks ──
  // Resolved through the retained hit registry (rects stored during paint
  // from hui layout nodes); no geometry is re-derived here.
  if (!app.rename_ui_open) return false;
  using hui::Hit::dialog;
  using hui::Hit::kDlgRename;
  using hui::Hit::kRenameCancel;
  using hui::Hit::kRenameInput;
  using hui::Hit::kRenameOk;
  const uint32_t hid = app.hit_main.query(x, y);

  // Right-click on input field → text context menu
  if (button == 0x111 && hid == dialog(kDlgRename, kRenameInput)) {
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

  if (button != 0x110) return true; // modal: swallow other buttons

    // Left-click on input field → position cursor + start drag
    if (hid == dialog(kDlgRename, kRenameInput)) {
      const hui::HitRegion* region = app.hit_main.find(x, y);
      int origin_x = region ? region->x : 0;
      cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t* cr_tmp = cairo_create(tmp);
      cairo_select_font_face(cr_tmp, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr_tmp, 14);
      int click_x = x - origin_x - 12;
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

    if (hid == dialog(kDlgRename, kRenameOk)) {
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

    if (hid == dialog(kDlgRename, kRenameCancel)) {
      app.rename_ui_open = false;
      draw(app);
      return true;
    }

    if (hid != dialog(kDlgRename, 0)) {
      app.rename_ui_open = false;
      draw(app);
    }
    return true;
}

bool click_batch_rename(AppState& app, int x, int y, int button) {
  // ── Batch rename click handling ──
  // Resolved through the retained hit registry (rects stored during paint);
  // no geometry is re-derived here.
  if (!app.batch_rename_open) return false;
  if (button != 0x110) return true; // modal: swallow other buttons
  using hui::Hit::dialog;
  using hui::Hit::kBatchAdd;
  using hui::Hit::kBatchAddItemBase;
  using hui::Hit::kBatchCancel;
  using hui::Hit::kBatchFind;
  using hui::Hit::kBatchOk;
  using hui::Hit::kBatchReplace;
  using hui::Hit::kBatchTab0;
  using hui::Hit::kBatchTab1;
  using hui::Hit::kBatchTemplate;
  using hui::Hit::kDlgBatch;
  const uint32_t hid = app.hit_main.query(x, y);
  bool is_template = (app.batch_rename_mode == 0);

  // ── Mode tab clicks ──
  if (hid == dialog(kDlgBatch, kBatchTab0)) {
    if (app.batch_rename_mode != 0) {
      app.batch_rename_mode = 0;
      app.batch_rename_edit_focus = 0;
      app.batch_rename_show_add = false;
      draw(app);
    }
    return true;
  }
  if (hid == dialog(kDlgBatch, kBatchTab1)) {
    if (app.batch_rename_mode != 1) {
      app.batch_rename_mode = 1;
      app.batch_rename_edit_focus = 0;
      draw(app);
    }
    return true;
  }

  if (is_template) {
    // ── Template field click ──
    if (hid == dialog(kDlgBatch, kBatchTemplate)) {
      app.batch_rename_edit_focus = 0;
      app.batch_rename_show_add = false;
      // Position cursor based on click
      app.batch_rename_template_cursor = static_cast<int>(app.batch_rename_template.size());
      draw(app);
      return true;
    }

    // ── [+ Add] button click ──
    if (hid == dialog(kDlgBatch, kBatchAdd)) {
      app.batch_rename_show_add = !app.batch_rename_show_add;
      app.batch_rename_add_hover = -1;
      draw(app);
      return true;
    }

    // ── [+ Add] dropdown option click ──
    if (app.batch_rename_show_add) {
      int ctrl = hui::Hit::dialog_ctrl(hid);
      int option = ctrl - kBatchAddItemBase;
      if (option >= 0 && option <= 2) {
        const char* inserts[] = {"[1]", "[01]", "[001]"};
        app.batch_rename_template.insert(app.batch_rename_template_cursor, inserts[option]);
        app.batch_rename_template_cursor += static_cast<int>(std::strlen(inserts[option]));
        app.batch_rename_show_add = false;
        draw(app);
        return true;
      }
      // Click outside dropdown closes it (then the click keeps processing)
      if (hid != dialog(kDlgBatch, kBatchAdd)) {
        app.batch_rename_show_add = false;
        draw(app);
      }
    }
  } else {
    // ── Find mode field clicks ──
    if (hid == dialog(kDlgBatch, kBatchFind)) {
      app.batch_rename_edit_focus = 0;
      app.batch_rename_find_cursor = static_cast<int>(app.batch_rename_find.size());
      draw(app);
      return true;
    }
    if (hid == dialog(kDlgBatch, kBatchReplace)) {
      app.batch_rename_edit_focus = 1;
      app.batch_rename_replace_cursor = static_cast<int>(app.batch_rename_replace.size());
      draw(app);
      return true;
    }
  }

  // ── Rename button ──
  if (hid == dialog(kDlgBatch, kBatchOk)) {
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
  if (hid == dialog(kDlgBatch, kBatchCancel) || hid != dialog(kDlgBatch, 0)) {
    app.batch_rename_open = false;
    draw(app);
    return true;
  }
  return true;
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

      // App list (rows resolved through the retained hit registry).
      {
        const uint32_t list_hid = app.hit_main.query(x, y);
        const int total = static_cast<int>(app.open_with_apps.size());
        int ctrl = hui::Hit::dialog_ctrl(list_hid);
        int row = ctrl - hui::Hit::kOpenRowBase;
        if (row >= 0 && row < total &&
            (list_hid & 0xFFFFC00) == hui::Hit::dialog(hui::Hit::kDlgOpenWith, 0)) {
          app.open_with_selected = row;
          if (app.open_with_set_default) {
            set_mime_default_app(app.open_with_mime, app.open_with_apps[row].desktop_id);
          }
          launch(app.open_with_apps[row]);
          open_with_close(app);
          draw(app);
          return true;
        }
      }
    }
    return true;
  }
  return false;
}

bool click_term_chooser(AppState& app, int x, int y, int button) {
  // ── Terminal chooser clicks ──
  // Resolved through the retained hit registry (rects stored during paint);
  // no geometry is re-derived here.
  if (!app.term_chooser_open) return false;
  if (button != 0x110) return true; // modal: swallow other buttons
  using hui::Hit::dialog;
  using hui::Hit::kDlgTerm;
  using hui::Hit::kTermClose;
  using hui::Hit::kTermRowBase;
  const uint32_t hid = app.hit_main.query(x, y);
  const int total = static_cast<int>(app.term_chooser_apps.size());

  if (hid == dialog(kDlgTerm, kTermClose) || hid != dialog(kDlgTerm, 0)) {
    // Close button or outside the card: dismiss.
    app.term_chooser_open = false;
    draw(app);
    return true;
  }

  int ctrl = hui::Hit::dialog_ctrl(hid);
  int item_idx = ctrl - kTermRowBase;
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
  return true;
}

} // namespace eh::file_browser
