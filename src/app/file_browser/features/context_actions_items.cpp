// context_actions_items.cpp — Exported from features/context_actions.cpp as part of the Step 6 file split.

#include "../app.hpp"
#include "app/file_browser/features/compare.hpp"
#include "app/file_browser/features/compress.hpp"
#include "app/file_browser/features/dirprops.hpp"
#include "app/file_browser/features/progress.hpp"
#include "app/file_browser/features/selection.hpp"
#include "app/file_browser/features/tab_history.hpp"
#include "app/file_browser/features/tags.hpp"
#include "app/file_browser/features/view_zoom.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config/shell_config.hpp"
#include "base/thread/thread_dispatch.hpp"
#include "platform/common/palette/matugen_palette.hpp"
#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "dialog/file_chooser_dialog.hpp"
#include "platform/widgets/app_drawer/list/desktop_list.hpp"
#include "services/udisks2/udisks2_drive_service.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;
using menu_clock = std::chrono::steady_clock;


namespace eh::file_browser {

// Per-item action dispatch: the switch from execute_context_menu_action. Runs
// AFTER the dispatcher resolved `entry`; a case that `return`s exits without
// drawing; a case that falls through (`break;`) reaches draw(app) below — matching
// the original single-function fallthrough exactly.
void execute_item_action(AppState& app, FileEntry& entry, AppState::ContextMenuAction action) {
  switch (action) {
    case AppState::ContextMenuAction::Open:
      if (entry.is_dir) {
        navigate_to(app, entry.path);
      } else {
        xdg::open_path_in_default_application(entry.path);
      }
      break;

    case AppState::ContextMenuAction::OpenWith:
      open_with_open(app, entry.path);
      draw(app);
      return;

    case AppState::ContextMenuAction::RunProgram: {
      pid_t pid = fork();
      if (pid == 0) {
        setsid();
        execlp(entry.path.c_str(), entry.path.c_str(), nullptr);
        _exit(1);
      }
      break;
    }

    case AppState::ContextMenuAction::Cut: {
      if (app.cur_tab().multi_selected.size() > 1) {
        std::vector<std::string> paths;
        for (int vis_idx : app.cur_tab().multi_selected) {
          if (vis_idx >= 0 && vis_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
            int r = app.cur_tab().visible_entries[vis_idx];
            if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size()))
              paths.push_back(app.cur_tab().entries[r].path);
          }
        }
        if (!paths.empty()) app.clipboard.copy_files(true, paths);
      } else {
        app.clipboard.copy_files(true, {entry.path});
      }
      app.operation_status = "Cut to clipboard";
      app.operation_status_expires_ms = menu_expiry_3s();
      break;
    }

    case AppState::ContextMenuAction::Copy: {
      if (app.cur_tab().multi_selected.size() > 1) {
        std::vector<std::string> paths;
        for (int vis_idx : app.cur_tab().multi_selected) {
          if (vis_idx >= 0 && vis_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
            int r = app.cur_tab().visible_entries[vis_idx];
            if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size()))
              paths.push_back(app.cur_tab().entries[r].path);
          }
        }
        if (!paths.empty()) app.clipboard.copy_files(false, paths);
      } else {
        app.clipboard.copy_files(false, {entry.path});
      }
      app.operation_status = "Copied to clipboard";
      app.operation_status_expires_ms = menu_expiry_3s();
      break;
    }

    case AppState::ContextMenuAction::PasteInto: {
      if (app.context_menu_file_idx >= 0 &&
          app.context_menu_file_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
        int r = app.cur_tab().visible_entries[app.context_menu_file_idx];
        if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size()) &&
            app.cur_tab().entries[r].is_dir)
          paste_clipboard(app, app.cur_tab().entries[r].path);
      }
      return;
    }

    case AppState::ContextMenuAction::InvertSelection:
      invert_selection(app);
      draw(app);
      return;

    case AppState::ContextMenuAction::HideFile:
    case AppState::ContextMenuAction::UnhideFile: {
      if (app.context_menu_file_idx >= 0 &&
          app.context_menu_file_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
        int r = app.cur_tab().visible_entries[app.context_menu_file_idx];
        if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size())) {
          const std::string& name = app.cur_tab().entries[r].name;
          std::string hidden_path = app.cur_tab().current_path + "/.hidden";
          std::vector<std::string> lines;
          {
            std::ifstream in(hidden_path);
            std::string line;
            while (std::getline(in, line)) {
              while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
              if (!line.empty()) lines.push_back(line);
            }
          }
          if (action == AppState::ContextMenuAction::HideFile) {
            if (std::find(lines.begin(), lines.end(), name) == lines.end())
              lines.push_back(name);
          } else {
            lines.erase(std::remove(lines.begin(), lines.end(), name), lines.end());
          }
          std::error_code ec;
          if (lines.empty()) {
            fs::remove(hidden_path, ec);
          } else {
            std::ofstream out(hidden_path, std::ios::trunc);
            for (const auto& l : lines) out << l << "\n";
          }
          reload_dir(app);
        }
      }
      draw(app);
      return;
    }

    case AppState::ContextMenuAction::SelectPattern:
      open_select_pattern(app);
      draw(app);
      return;

    case AppState::ContextMenuAction::CompareFiles:
      compare_selected_files(app);
      return;

    case AppState::ContextMenuAction::ApplyPropsToSubfolders: {
      // Snapshot the current pane's view properties, then write them into
      // every subfolder of the selected directory (background thread).
      int vi = app.cur_tab().selected_idx;
      if (vi < 0 || vi >= static_cast<int>(app.cur_tab().visible_entries.size())) return;
      int ri2 = app.cur_tab().visible_entries[vi];
      if (ri2 < 0 || ri2 >= static_cast<int>(app.cur_tab().entries.size())) return;
      std::string root_dir = app.cur_tab().entries[ri2].path;

      DirProps p;
      p.has_mode = true;
      p.mode = static_cast<int>(app.cur_tab().view_mode);
      p.has_sort = true;
      p.sort_field = static_cast<int>(app.cur_tab().sort_field);
      p.sort_descending = app.cur_tab().sort_descending;
      p.has_flags = true;
      p.natural = app.sort_natural;
      p.case_sensitive = app.sort_case_sensitive;
      p.hidden_last = app.sort_hidden_last;
      p.folders_first = app.folders_before_files;
      p.has_group = true;
      p.group_field = app.cur_tab().group_field;
      p.has_zoom = true;
      p.zoom_level = zoom_level_for_pct(app.zoom_pct);
      p.has_hidden = true;
      p.show_hidden = app.show_hidden;

      auto prog = std::make_shared<OperationProgress>();
      prog->type = OperationType::Extract;
      prog->active.store(true);
      app.op_progress = prog;
      app.ops_panel_open = true;
      app.operation_status = "Applying view properties...";
      draw(app);

      std::thread([&app, prog, root_dir, p]() {
        int written =
            apply_dir_props_recursive(root_dir, p, [&]() { return prog->cancel.load(); });
        bool cancelled = prog->cancel.load();
        DeferredCall::callLater([&app, cancelled, written]() {
          if (cancelled)
            app.operation_status = "Apply cancelled";
          else
            app.operation_status = "Applied view properties to " +
                                   std::to_string(written) + " folders";
          app.operation_status_expires_ms =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count() + 3000;
          draw(app);
        });
      }).detach();
      return;
    }

    case AppState::ContextMenuAction::CopyToHome:
    case AppState::ContextMenuAction::CopyToDesktop:
    case AppState::ContextMenuAction::CopyToOtherPane: {
      std::string dest;
      if (action == AppState::ContextMenuAction::CopyToHome)
        dest = home_dir();
      else if (action == AppState::ContextMenuAction::CopyToDesktop)
        dest = home_dir() + "/Desktop";
      else
        dest = app.active_pane == 0 ? app.right_pane.current_path
                                    : app.tabs[app.active_tab].current_path;
      request_fs_operation(app, selected_entry_paths(app), dest, false,
                           "Copied to destination", false);
      return;
    }

    case AppState::ContextMenuAction::MoveToHome:
    case AppState::ContextMenuAction::MoveToDesktop:
    case AppState::ContextMenuAction::MoveToOtherPane: {
      std::string dest;
      if (action == AppState::ContextMenuAction::MoveToHome)
        dest = home_dir();
      else if (action == AppState::ContextMenuAction::MoveToDesktop)
        dest = home_dir() + "/Desktop";
      else
        dest = app.active_pane == 0 ? app.right_pane.current_path
                                    : app.tabs[app.active_tab].current_path;
      request_fs_operation(app, selected_entry_paths(app), dest, true,
                           "Moved to destination", false);
      return;
    }

    case AppState::ContextMenuAction::CopyTo: {
      std::string dest;
      if (!eh::dialog::show_native_folder_picker(&dest)) { draw(app); return; }
      if (!dest.empty()) {
        std::vector<std::string> src_paths;
        for (int vis_idx : app.cur_tab().multi_selected) {
          if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size())) continue;
          int r = app.cur_tab().visible_entries[vis_idx];
          if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size()))
            src_paths.push_back(app.cur_tab().entries[r].path);
        }
        request_fs_operation(app, src_paths, dest, false, "Copied to destination", false);
      }
      draw(app);
      return;
    }

    case AppState::ContextMenuAction::MoveTo: {
      std::string dest;
      if (!eh::dialog::show_native_folder_picker(&dest)) { draw(app); return; }
      if (!dest.empty()) {
        std::vector<std::string> src_paths;
        for (int vis_idx : app.cur_tab().multi_selected) {
          if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size())) continue;
          int r = app.cur_tab().visible_entries[vis_idx];
          if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size()))
            src_paths.push_back(app.cur_tab().entries[r].path);
        }
        request_fs_operation(app, src_paths, dest, true, "Moved to destination", false);
      }
      draw(app);
      return;
    }

    case AppState::ContextMenuAction::Rename: {
      bool is_multi = app.cur_tab().multi_selected.size() > 1;
      if (is_multi) {
        // Batch rename via right-click
        app.batch_rename_entries.clear();
        auto all = app.cur_tab().multi_selected;
        // Ensure the right-clicked entry is included
        bool has_clicked = false;
        for (int vis_idx : all) {
          if (vis_idx == app.context_menu_file_idx) { has_clicked = true; break; }
        }
        if (!has_clicked && app.context_menu_file_idx >= 0)
          all.push_back(app.context_menu_file_idx);
        for (int vis_idx : all) {
          if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size())) continue;
          int r = app.cur_tab().visible_entries[vis_idx];
          if (r < 0 || r >= static_cast<int>(app.cur_tab().entries.size())) continue;
          const auto& e2 = app.cur_tab().entries[r];
          std::string ext;
          auto dot = e2.name.rfind('.');
          if (dot == std::string::npos || dot == 0) ext.clear();
          else ext = e2.name.substr(dot);
          app.batch_rename_entries.push_back({e2.path, e2.name, ext, e2.name});
        }
        if (!app.batch_rename_entries.empty()) {
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
        }
      } else {
        app.rename_ui_open = true;
        app.rename_ui_old_name = entry.name;
        app.rename_ui_buf = entry.name;
        app.rename_ui_cursor_pos = static_cast<int>(entry.name.size());
        app.rename_ui_entry_path = entry.path;
        app.rename_ui_hover_btn = -1;
      }
      break;
    }

    case AppState::ContextMenuAction::RestoreFromTrash: {
      std::vector<std::string> restore_paths;
      bool is_multi = app.cur_tab().multi_selected.size() > 1;
      if (is_multi) {
        for (int vis_idx : app.cur_tab().multi_selected) {
          if (vis_idx >= 0 && vis_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
            int r = app.cur_tab().visible_entries[vis_idx];
            if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size()))
              restore_paths.push_back(app.cur_tab().entries[r].path);
          }
        }
      } else {
        restore_paths.push_back(entry.path);
      }
      int restored = 0;
      for (const auto& p : restore_paths) {
        if (xdg::restore_from_trash(p)) ++restored;
      }
      reload_dir(app);
      if (restored > 0) {
        app.operation_status = "Restored " + std::to_string(restored) + " item" + (restored > 1 ? "s" : "");
      } else {
        app.operation_status = "Restore failed";
      }
      app.operation_status_expires_ms = menu_expiry_3s();
      break;
    }

    case AppState::ContextMenuAction::MoveToTrash: {
      {
        bool is_multi = app.cur_tab().multi_selected.size() > 1;
        int count = is_multi ? static_cast<int>(app.cur_tab().multi_selected.size()) : 1;
        app.confirm_title = "Move to Trash";
        if (count == 1)
          app.confirm_message = "Move \"" + entry.name + "\" to trash?";
        else
          app.confirm_message = "Move " + std::to_string(count) + " items to trash?";
        app.confirm_preview_path = count == 1 ? entry.path : "";
        app.confirm_item_count = count;
        std::vector<std::string> trash_paths;
        if (is_multi) {
          for (int vis_idx : app.cur_tab().multi_selected) {
            if (vis_idx >= 0 && vis_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
              int r = app.cur_tab().visible_entries[vis_idx];
              if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size()))
                trash_paths.push_back(app.cur_tab().entries[r].path);
            }
          }
        } else {
          trash_paths.push_back(entry.path);
        }
        app.confirm_callback = [&app, paths = std::move(trash_paths)](bool ok) {
          if (!ok) return;
          for (const auto& p : paths) (void)xdg::trash_file(p);
          reload_dir(app);
          app.operation_status = "Moved to trash";
          app.operation_status_expires_ms = menu_expiry_3s();
        };
      }
      app.confirm_hover_btn = -1;
      app.confirm_open = true;
      draw(app);
      break;
    }

    case AppState::ContextMenuAction::PermanentDelete: {
      {
        bool is_multi = app.cur_tab().multi_selected.size() > 1;
        int count = is_multi ? static_cast<int>(app.cur_tab().multi_selected.size()) : 1;
        app.confirm_title = "Permanently Delete?";
        if (count == 1)
          app.confirm_message = "Permanently delete \"" + entry.name + "\"?";
        else
          app.confirm_message = "Permanently delete " + std::to_string(count) + " items?";
        app.confirm_preview_path = count == 1 ? entry.path : "";
        app.confirm_item_count = count;
        std::vector<std::pair<fs::path, bool>> delete_paths;
        if (is_multi) {
          for (int vis_idx : app.cur_tab().multi_selected) {
            if (vis_idx >= 0 && vis_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
              int r = app.cur_tab().visible_entries[vis_idx];
              if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size()))
                delete_paths.emplace_back(app.cur_tab().entries[r].path, app.cur_tab().entries[r].is_dir);
            }
          }
        } else {
          delete_paths.emplace_back(entry.path, entry.is_dir);
        }
        app.confirm_callback = [&app, paths = std::move(delete_paths)](bool ok) {
          if (!ok) return;
          std::error_code ec;
          for (const auto& [p, is_dir] : paths) {
            if (is_dir) fs::remove_all(p, ec); else fs::remove(p, ec);
          }
          reload_dir(app);
          app.operation_status = "Permanently deleted";
          app.operation_status_expires_ms = menu_expiry_3s();
        };
      }
      app.confirm_hover_btn = -1;
      app.confirm_open = true;
      draw(app);
      break;
    }

    case AppState::ContextMenuAction::BrowseArchive: {
      pid_t pid = fork();
      if (pid == 0) {
        execlp("horizon-archive", "horizon-archive", entry.path.c_str(), nullptr);
        _exit(1);
      }
      draw(app);
      return;
    }

    case AppState::ContextMenuAction::Extract: {
      for (int vis_idx : app.cur_tab().multi_selected) {
        if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size())) continue;
        int r = app.cur_tab().visible_entries[vis_idx];
        if (r < 0 || r >= static_cast<int>(app.cur_tab().entries.size())) continue;
        execute_extract_async(app, app.cur_tab().entries[r].path,
                              default_extract_dir(app.cur_tab().entries[r].path));
      }
      draw(app);
      return;
    }

    case AppState::ContextMenuAction::ExtractTo: {
      std::string dest;
      if (!eh::dialog::show_native_folder_picker(&dest)) { draw(app); return; }
      if (!dest.empty()) {
        for (int vis_idx : app.cur_tab().multi_selected) {
          if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size())) continue;
          int r = app.cur_tab().visible_entries[vis_idx];
          if (r < 0 || r >= static_cast<int>(app.cur_tab().entries.size())) continue;
          execute_extract_async(app, app.cur_tab().entries[r].path, dest);
        }
      }
      draw(app);
      return;
    }

    case AppState::ContextMenuAction::MountIso: {
      std::string iso = entry.path;
      app.operation_status = "Mounting disk image...";
      app.operation_status_expires_ms = menu_expiry_3s();
      draw(app);
      drives::UDisks2DriveService::instance().mount_iso_async(
          iso, [&app, iso](bool ok, std::string mnt) {
            DeferredCall::callLater([&app, ok, mnt, iso]() {
              if (ok && !mnt.empty()) {
                app.operation_status = "Mounted";
                app.operation_status_expires_ms = menu_expiry_3s();
                app.sidebar_needs_refresh = true;
                app.computer_needs_refresh = true;
                navigate_to(app, mnt);
              } else {
                app.operation_status = "Mount failed";
                app.operation_status_expires_ms = menu_expiry_3s();
                draw(app);
              }
            });
          });
      draw(app);
      return;
    }

    case AppState::ContextMenuAction::UnmountIso: {
      std::string iso = entry.path;
      app.operation_status = "Unmounting disk image...";
      app.operation_status_expires_ms = menu_expiry_3s();
      draw(app);
      drives::UDisks2DriveService::instance().unmount_iso_async(
          iso, [&app](bool ok) {
            DeferredCall::callLater([&app, ok]() {
              app.operation_status = ok ? "Unmounted" : "Unmount failed";
              app.operation_status_expires_ms = menu_expiry_3s();
              app.sidebar_needs_refresh = true;
              app.computer_needs_refresh = true;
              reload_dir(app);
              draw(app);
            });
          });
      draw(app);
      return;
    }

    case AppState::ContextMenuAction::Compress: {
      std::vector<std::string> paths;
      if (app.cur_tab().multi_selected.size() > 1) {
        for (int vis_idx : app.cur_tab().multi_selected) {
          if (vis_idx >= 0 && vis_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
            int r = app.cur_tab().visible_entries[vis_idx];
            if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size()))
              paths.push_back(app.cur_tab().entries[r].path);
          }
        }
      } else {
        paths.push_back(entry.path);
      }
      app.compress_source_paths = paths;
      app.compress_source_name = entry.name;
      // Strip extension for default archive name
      {
        std::string stem = entry.name;
        auto dot = stem.rfind('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        app.compress_name_buf = stem;
      }
      check_compress_tool_availability(app);
      app.compress_name_cursor = static_cast<int>(app.compress_name_buf.size());
      app.compress_format = 1; // tar.gz
      app.compress_level = 6;
      app.compress_threads = 0; // Auto
      app.compress_hover_format = -1;
      app.compress_hover_level = -1;
      app.compress_hover_threads = -1;
      app.compress_hover_btn = -1;
      app.compress_dialog_open = true;
      draw(app);
      return;
    }

    case AppState::ContextMenuAction::Duplicate: {
      {
        std::error_code ec;
        int dup_count = 0;
        for (int vis_idx : app.cur_tab().multi_selected) {
          if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size())) continue;
          int r = app.cur_tab().visible_entries[vis_idx];
          if (r < 0 || r >= static_cast<int>(app.cur_tab().entries.size())) continue;
          auto& e = app.cur_tab().entries[r];
          fs::path src(e.path);
          fs::path parent = src.parent_path();
          fs::path target = parent / (src.stem().string() + " (copy)" + src.extension().string());
          int n = 2;
          while (fs::exists(target, ec)) {
            target = parent / (src.stem().string() + " (" + std::to_string(n) + ")" + src.extension().string());
            n++;
          }
          if (e.is_dir)
            fs::copy(src, target, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
          else
            fs::copy_file(src, target, fs::copy_options::copy_symlinks, ec);
          if (!ec) ++dup_count;
        }
        if (dup_count > 0) {
          reload_dir(app);
          app.operation_status = "Duplicated";
          app.operation_status_expires_ms = menu_expiry_3s();
        }
      }
      draw(app);
      return;
    }
    case AppState::ContextMenuAction::CreateSymlink: {
      std::string dest;
      if (!eh::dialog::show_native_folder_picker(&dest)) { draw(app); return; }
      if (!dest.empty()) {
        std::error_code ec;
        int link_count = 0;
        for (int vis_idx : app.cur_tab().multi_selected) {
          if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size())) continue;
          int r = app.cur_tab().visible_entries[vis_idx];
          if (r < 0 || r >= static_cast<int>(app.cur_tab().entries.size())) continue;
          auto& e = app.cur_tab().entries[r];
          fs::path src(e.path);
          fs::path link_path = fs::path(dest) / (e.name + " (link)");
          int n = 2;
          while (fs::exists(link_path, ec)) {
            link_path = fs::path(dest) / (e.name + " (link " + std::to_string(n) + ")");
            n++;
          }
          if (e.is_dir)
            fs::create_directory_symlink(src, link_path, ec);
          else
            fs::create_symlink(src, link_path, ec);
          if (!ec) ++link_count;
        }
        if (link_count > 0) {
          reload_dir(app);
          app.operation_status = "Symlink created";
          app.operation_status_expires_ms = menu_expiry_3s();
        }
      }
      draw(app);
      return;
    }
    case AppState::ContextMenuAction::CopyPath: {
      std::string joined;
      for (int vis_idx : app.cur_tab().multi_selected) {
        if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size())) continue;
        int r = app.cur_tab().visible_entries[vis_idx];
        if (r < 0 || r >= static_cast<int>(app.cur_tab().entries.size())) continue;
        if (!joined.empty()) joined += '\n';
        joined += app.cur_tab().entries[r].path;
      }
      if (joined.empty()) joined = entry.path;
      app.clipboard.copy_text(joined);
      app.operation_status = "Path copied";
      app.operation_status_expires_ms = menu_expiry_3s();
      break;
    }
    case AppState::ContextMenuAction::Properties: {
      bool is_multi = app.cur_tab().multi_selected.size() > 1;
      if (is_multi) {
        std::vector<std::string> paths;
        for (int vis_idx : app.cur_tab().multi_selected) {
          if (vis_idx < 0 || vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size())) continue;
          int r = app.cur_tab().visible_entries[vis_idx];
          if (r < 0 || r >= static_cast<int>(app.cur_tab().entries.size())) continue;
          paths.push_back(app.cur_tab().entries[r].path);
        }
        show_properties_multi(app, paths);
      } else {
        show_properties(app, entry.path, entry.icon_name);
      }
      break;
    }

    case AppState::ContextMenuAction::Settings:
      break;

    default:
      break;
  }
  draw(app);
}

} // namespace eh::file_browser
