// context_actions.cpp — Exported from features/menu.cpp as part of the Step 5 file split.

#include "../../app.hpp"
#include "app/file_browser/features/compare/compare.hpp"
#include "app/file_browser/features/compress/compress.hpp"
#include "app/file_browser/features/dirprops/dirprops.hpp"
#include "app/file_browser/features/progress/progress.hpp"
#include "app/file_browser/features/selection/selection.hpp"
#include "app/file_browser/features/tab_history/tab_history.hpp"
#include "app/file_browser/features/tags/tags.hpp"
#include "app/file_browser/features/view_zoom/view_zoom.hpp"

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

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;
using menu_clock = std::chrono::steady_clock;

namespace eh::file_browser {

std::uint64_t menu_expiry_3s() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             (menu_clock::now() + std::chrono::milliseconds(3000)).time_since_epoch())
      .count();
}

// Paths of the current multi-selection (falls back to the single selection).
std::vector<std::string> selected_entry_paths(eh::file_browser::AppState& app) {
  std::vector<std::string> paths;
  auto& tab = app.cur_tab();
  for (int vis_idx : tab.multi_selected) {
    if (vis_idx < 0 || vis_idx >= static_cast<int>(tab.visible_entries.size())) continue;
    int r = tab.visible_entries[vis_idx];
    if (r >= 0 && r < static_cast<int>(tab.entries.size()))
      paths.push_back(tab.entries[r].path);
  }
  return paths;
}

// ── context menu action execution ────────────────────────────────

void execute_context_menu_action(AppState& app, int item_idx) {
  if (item_idx < 0 ||
      item_idx >= static_cast<int>(app.context_menu_items.size()))
    return;

  auto action = app.context_menu_items[item_idx].action;
  app.context_menu_open = false;

  if (action == AppState::ContextMenuAction::Separator)
    return;

  // ── Path editing context menu actions ──
  if (ctx_path_edit_menu(app, item_idx, action)) return;
  // ── Dialog text editing context menu actions ──
  if (ctx_dialog_text_menu(app, item_idx, action)) return;
  if (ctx_new_folder(app, item_idx, action)) return;
  if (ctx_new_document(app, item_idx, action)) return;
  if (ctx_new_from_template(app, item_idx, action)) return;
  if (ctx_run_script(app, item_idx, action)) return;
  if (ctx_reload(app, item_idx, action)) return;
  if (ctx_copy_location(app, item_idx, action)) return;
  if (ctx_select_all(app, item_idx, action)) return;
  // ── Dots menu "Open With…" — use selected file if any ──
  if (ctx_open_with(app, item_idx, action)) return;
  // ── Background/dots menu "Properties" — current directory ──
  if (ctx_properties(app, item_idx, action)) return;
  if (ctx_paste(app, item_idx, action)) return;
  if (ctx_open_in_terminal(app, item_idx, action)) return;
  if (ctx_remove_from_favorites(app, item_idx, action)) return;
  if (ctx_unmount_drive(app, item_idx, action)) return;
  if (ctx_mount_drive(app, item_idx, action)) return;
  // ── Open Trash in current tab ──
  if (ctx_open(app, item_idx, action)) return;
  // ── Empty Trash ──
  if (ctx_empty_trash(app, item_idx, action)) return;
  if (ctx_add_to_favorites(app, item_idx, action)) return;
  if (ctx_settings(app, item_idx, action)) return;
  // ── Open in new tab ──
  if (ctx_open_in_new_tab(app, item_idx, action)) return;
  // ── Tab context menu actions ──
  if (ctx_tab_menu(app, item_idx, action)) return;
  // ── Open in new window ──
  if (ctx_open_in_new_window(app, item_idx, action)) return;
  // ── Open as Administrator ──
  if (ctx_open_as_admin(app, item_idx, action)) return;
  // ── Open file location (navigate to parent dir) ──
  if (ctx_open_file_location(app, item_idx, action)) return;

  // Tree-row menus carry their own materialized target: expanded rows have
  // no slot in visible_entries, so index resolution would bind (and act!)
  // on the wrong file. Refresh existence so stale targets fail safely.
  FileEntry* entry_target = nullptr;
  if (app.context_menu_file_idx == -9) {
    if (app.context_menu_tree_entry.path.empty()) return;
    std::error_code tec;
    app.context_menu_tree_entry.is_dir =
        fs::is_directory(app.context_menu_tree_entry.path, tec);
    entry_target = &app.context_menu_tree_entry;
  } else {
    if (app.context_menu_file_idx < 0 ||
        app.context_menu_file_idx >= static_cast<int>(app.cur_tab().visible_entries.size()))
      return;
    int real_idx = app.cur_tab().visible_entries[app.context_menu_file_idx];
    if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
      return;
    entry_target = &app.cur_tab().entries[real_idx];
  }
  auto& entry = *entry_target;


  execute_item_action(app, entry, action);
}

} // namespace eh::file_browser
