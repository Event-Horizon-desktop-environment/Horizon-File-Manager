// context_actions.cpp — Exported from features/menu.cpp as part of the Step 5 file split.

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
static std::vector<std::string> selected_entry_paths(eh::file_browser::AppState& app) {
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
// Percent-encoding file URI (mirrors ClipboardService/drag encoders).
static std::string menu_file_uri(const std::string& abs_path) {
  std::string out = "file://";
  char buf[8];
  for (unsigned char c : abs_path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
        c == '.' || c == '~')
      out += static_cast<char>(c);
    else if (c == ' ')
      out += "%20";
    else {
      std::snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// Launches `script` detached (double-fork, no zombie) with the documented
// NEMO_SCRIPT_* environment contract; selection is also passed as argv and
// the working directory is set to the current folder.
static void run_nemo_script(AppState& app, const std::string& script_path) {
  const auto sel = selected_entry_paths(app);
  const std::string cwd = app.cur_tab().current_path;

  std::string paths_nl, uris_nl;
  for (std::size_t i = 0; i < sel.size(); ++i) {
    if (!paths_nl.empty()) {
      paths_nl += '\n';
      uris_nl += '\n';
    }
    paths_nl += sel[i];
    uris_nl += menu_file_uri(sel[i]);
  }

  std::vector<std::string> extra;
  extra.push_back("NEMO_SCRIPT_SELECTED_FILE_PATHS=" + paths_nl);
  extra.push_back("NEMO_SCRIPT_SELECTED_URIS=" + uris_nl);
  extra.push_back("NEMO_SCRIPT_CURRENT_URI=" +
                  menu_file_uri(cwd));
  extra.push_back("NEMO_SCRIPT_WINDOW_GEOMETRY=0 0 " +
                  std::to_string(app.width) + " " + std::to_string(app.height));
  if (app.split_view) {
    const Tab& other = app.active_pane == 1 ? app.tabs[app.active_tab]
                                            : app.right_pane;
    std::string opaths, ouris;
    for (int vi : other.multi_selected) {
      if (vi < 0 || vi >= static_cast<int>(other.visible_entries.size())) continue;
      int r = other.visible_entries[vi];
      if (r < 0 || r >= static_cast<int>(other.entries.size())) continue;
      if (!opaths.empty()) {
        opaths += '\n';
        ouris += '\n';
      }
      opaths += other.entries[r].path;
      ouris += menu_file_uri(other.entries[r].path);
    }
    extra.push_back("NEMO_SCRIPT_NEXT_PANE_SELECTED_FILE_PATHS=" + opaths);
    extra.push_back("NEMO_SCRIPT_NEXT_PANE_SELECTED_URIS=" + ouris);
    extra.push_back("NEMO_SCRIPT_NEXT_PANE_CURRENT_URI=" +
                    menu_file_uri(other.current_path));
  }

  // envp = current environ with any stale NEMO_SCRIPT_* stripped + extras
  std::vector<char*> envp;
  for (char** e = environ; e && *e; ++e)
    if (!std::string_view(*e).starts_with("NEMO_SCRIPT_"))
      envp.push_back(*e);
  for (const auto& s : extra) envp.push_back(const_cast<char*>(s.c_str()));
  envp.push_back(nullptr);

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(script_path.c_str()));
  for (const auto& p : sel) argv.push_back(const_cast<char*>(p.c_str()));
  argv.push_back(nullptr);

  pid_t mid = ::fork();
  if (mid < 0) return;
  if (mid == 0) {
    pid_t pid2 = ::fork();
    if (pid2 == 0) {
      ::chdir(cwd.c_str());
      ::execve(script_path.c_str(), argv.data(), envp.data());
      _exit(127);
    }
    _exit(pid2 > 0 ? 0 : 127);
  }
  int st = 0;
  ::waitpid(mid, &st, 0);
}

// ── Open as Administrator ────────────────────────────────────────

// Re-launches the browser as root over pkexec pointed at `target_dir`.
// pkexec scrubs the environment, so the Wayland session variables the
// elevated instance needs are re-exported explicitly.
//
// Spawned with a plain single fork and NOT waited on: pkexec refuses to
// run ("Refusing to render service to dead parents") when its parent is
// gone or exits immediately, so daemonizing via double-fork is not an
// option — this process stays alive as its parent instead.
static void open_as_admin(const std::string& target_dir) {
  const char* wd = std::getenv("WAYLAND_DISPLAY");
  if (!wd || !wd[0]) wd = "wayland-0";
  const char* xrd = std::getenv("XDG_RUNTIME_DIR");
  if (!xrd || !xrd[0]) return;

  char exe_buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
  if (n <= 0) return;
  exe_buf[static_cast<std::size_t>(n)] = '\0';

  std::string wd_env = "WAYLAND_DISPLAY=" + std::string(wd);
  std::string xrd_env = "XDG_RUNTIME_DIR=" + std::string(xrd);

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("pkexec"));
  argv.push_back(const_cast<char*>("env"));
  argv.push_back(wd_env.data());
  argv.push_back(xrd_env.data());
  argv.push_back(exe_buf);
  argv.push_back(const_cast<char*>(target_dir.c_str()));
  argv.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid < 0) return;
  if (pid == 0) {
    ::unsetenv("WAYLAND_SOCKET");
    ::execvp(argv[0], argv.data());
    _exit(127);
  }
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
  if (app.context_menu_file_idx == -3) {
    auto& m_buf = app.active_pane ? app.r_path_edit_buf : app.path_edit_buf;
    auto& m_sel_start = app.active_pane ? app.r_path_edit_sel_start : app.path_edit_sel_start;
    auto& m_sel_end = app.active_pane ? app.r_path_edit_sel_end : app.path_edit_sel_end;
    if (action == AppState::ContextMenuAction::Copy) {
      if (m_sel_start >= 0 && m_sel_start != m_sel_end) {
        int sel_a = std::min(m_sel_start, m_sel_end);
        int sel_b = std::max(m_sel_start, m_sel_end);
        std::string sel = m_buf.substr(sel_a, sel_b - sel_a);
        if (!sel.empty()) app.clipboard.copy_text(sel);
      } else {
        app.clipboard.copy_text(m_buf);
      }
      draw(app);
      return;
    }
  }

  // ── Dialog text editing context menu actions ──
  if (app.context_menu_file_idx == -6 || app.context_menu_file_idx == -7 ||
      app.context_menu_file_idx == -8) {
    // Determine which buffer/selection/cursor to use
    std::string* buf = nullptr;
    int* cursor = nullptr;
    int* sel_start = nullptr;
    int* sel_end = nullptr;
    if (app.context_menu_file_idx == -6) {
      buf = &app.create_buf;
      cursor = &app.create_cursor_pos;
      sel_start = &app.create_sel_start;
      sel_end = &app.create_sel_end;
    } else if (app.context_menu_file_idx == -7) {
      buf = &app.rename_ui_buf;
      cursor = &app.rename_ui_cursor_pos;
      sel_start = &app.rename_ui_sel_start;
      sel_end = &app.rename_ui_sel_end;
    } else {
      buf = &app.password_buf;
      cursor = &app.password_cursor_pos;
      sel_start = &app.password_sel_start;
      sel_end = &app.password_sel_end;
    }

    if (action == AppState::ContextMenuAction::Cut) {
      if (*sel_start >= 0 && *sel_start != *sel_end) {
        int a = std::min(*sel_start, *sel_end);
        int b = std::max(*sel_start, *sel_end);
        std::string sel = buf->substr(a, b - a);
        if (!sel.empty()) app.clipboard.copy_text(sel);
        buf->erase(a, b - a);
        *cursor = a;
        *sel_start = -1;
        *sel_end = -1;
      }
      draw(app);
      return;
    }
    if (action == AppState::ContextMenuAction::Copy) {
      if (*sel_start >= 0 && *sel_start != *sel_end) {
        int a = std::min(*sel_start, *sel_end);
        int b = std::max(*sel_start, *sel_end);
        std::string sel = buf->substr(a, b - a);
        if (!sel.empty()) app.clipboard.copy_text(sel);
      } else {
        app.clipboard.copy_text(*buf);
      }
      draw(app);
      return;
    }
    if (action == AppState::ContextMenuAction::Paste) {
      std::string clip = app.clipboard.read_selection_text(app.wl.display());
      if (!clip.empty()) {
        if (*sel_start >= 0 && *sel_start != *sel_end) {
          int a = std::min(*sel_start, *sel_end);
          int b = std::max(*sel_start, *sel_end);
          buf->erase(a, b - a);
          *cursor = a;
        }
        buf->insert(*cursor, clip);
        *cursor += static_cast<int>(clip.size());
        *sel_start = -1;
        *sel_end = -1;
      }
      draw(app);
      return;
    }
    if (action == AppState::ContextMenuAction::SelectAll) {
      *sel_start = 0;
      *sel_end = static_cast<int>(buf->size());
      *cursor = *sel_end;
      draw(app);
      return;
    }
  }

  if (action == AppState::ContextMenuAction::NewFolder) {
    app.create_dialog_open = true;
    app.create_is_folder = true;
    app.create_buf = "New Folder";
    app.create_cursor_pos = static_cast<int>(app.create_buf.size());
    app.create_hover_btn = -1;
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::NewDocument) {
    app.create_dialog_open = true;
    app.create_is_folder = false;
    app.create_buf = "New Document";
    app.create_cursor_pos = static_cast<int>(app.create_buf.size());
    app.create_hover_btn = -1;
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::NewFromTemplate) {
    const auto& item = app.context_menu_items[item_idx];
    app.create_dialog_open = true;
    app.create_is_folder = false;
    app.create_template_src = item.data;
    fs::path src(item.data);
    std::string stem = src.filename().string();
    std::string ext = src.extension().string();
    if (!ext.empty() && stem.size() > ext.size())
      stem.resize(stem.size() - ext.size());
    if (stem.empty()) stem = "New Document";
    app.create_buf = stem;
    app.create_cursor_pos = static_cast<int>(app.create_buf.size());
    app.create_hover_btn = -1;
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::RunScript) {
    run_nemo_script(app, app.context_menu_items[item_idx].data);
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::Reload) {
    reload_dir(app);
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::CopyLocation) {
    app.clipboard.copy_text(app.cur_tab().current_path);
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::SelectAll) {
    app.cur_tab().multi_selected.clear();
    for (int vi = 0; vi < static_cast<int>(app.cur_tab().visible_entries.size()); ++vi)
      app.cur_tab().multi_selected.push_back(vi);
    draw(app);
    return;
  }

  // ── Dots menu "Open With…" — use selected file if any ──
  if (action == AppState::ContextMenuAction::OpenWith) {
    int sel = app.cur_tab().selected_idx;
    if (sel >= 0 && sel < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real = app.cur_tab().visible_entries[sel];
      if (real >= 0 && real < static_cast<int>(app.cur_tab().entries.size()))
        open_with_open(app, app.cur_tab().entries[real].path);
    }
    draw(app);
    return;
  }

  // ── Background/dots menu "Properties" — current directory ──
  if (action == AppState::ContextMenuAction::Properties &&
      (app.context_menu_file_idx == -1 || app.context_menu_file_idx == -5)) {
    show_properties(app, app.cur_tab().current_path);
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::Paste) {
    paste_clipboard(app);
    return;
  }

  if (action == AppState::ContextMenuAction::OpenInTerminal) {
    std::string target_dir = app.cur_tab().current_path;
    if (app.context_menu_file_idx >= 0 &&
        app.context_menu_file_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real_idx = app.cur_tab().visible_entries[app.context_menu_file_idx];
      if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()) &&
          app.cur_tab().entries[real_idx].is_dir) {
        target_dir = app.cur_tab().entries[real_idx].path;
      }
    }
    open_terminal_at(app, target_dir);
    return;
  }

  if (action == AppState::ContextMenuAction::RemoveFromFavorites) {
    if (app.context_menu_sidebar_idx >= 0 &&
        app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
      const auto& loc = app.sidebar_locations[app.context_menu_sidebar_idx];
      auto it = std::find(app.favorites.begin(), app.favorites.end(), loc.path);
      if (it != app.favorites.end()) {
        app.favorites.erase(it);
        save_file_browser_settings(app);
        refresh_sidebar(app);
      }
    }
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::UnmountDrive) {
    if (app.context_menu_sidebar_idx >= 0 &&
        app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
      unmount_drive(app, app.context_menu_sidebar_idx);
    }
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::MountDrive) {
    if (app.context_menu_sidebar_idx >= 0 &&
        app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
      mount_drive(app, app.context_menu_sidebar_idx);
    }
    draw(app);
    return;
  }

  // ── Open Trash in current tab ──
  if (action == AppState::ContextMenuAction::Open &&
      app.context_menu_sidebar_idx >= 0 &&
      app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
    auto& loc = app.sidebar_locations[app.context_menu_sidebar_idx];
    if (loc.kind == SidebarLocation::Kind::Trash) {
      app.cur_tab().current_path = loc.path;
      navigate_to(app, loc.path);
      draw(app);
      return;
    }
  }

  // ── Empty Trash ──
  if (action == AppState::ContextMenuAction::EmptyTrash) {
    const char* home = std::getenv("HOME");
    if (home) {
      fs::path trash_dir(home);
      trash_dir /= ".local/share/Trash";
      std::error_code ec;
      fs::remove_all(trash_dir / "files", ec);
      fs::remove_all(trash_dir / "info", ec);
      fs::create_directories(trash_dir / "files", ec);
      fs::create_directories(trash_dir / "info", ec);
      app.operation_status = "Trash emptied";
      app.operation_status_expires_ms = menu_expiry_3s();
      reload_dir(app);
    }
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::AddToFavorites) {
    if (app.context_menu_file_idx >= 0 &&
        app.context_menu_file_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real_idx = app.cur_tab().visible_entries[app.context_menu_file_idx];
      if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()) &&
          app.cur_tab().entries[real_idx].is_dir) {
        const auto& path = app.cur_tab().entries[real_idx].path;
        if (std::find(app.favorites.begin(), app.favorites.end(), path) == app.favorites.end()) {
          app.favorites.push_back(path);
          save_file_browser_settings(app);
          refresh_sidebar(app);
        }
      }
    }
    draw(app);
    return;
  }

  if (action == AppState::ContextMenuAction::Settings) {
    open_settings(app);
    draw(app);
    return;
  }

  // ── Open in new tab ──
  if (action == AppState::ContextMenuAction::OpenInNewTab) {
    std::string target_dir;
    if (app.context_menu_file_idx == -2) {
      // Sidebar item
      if (app.context_menu_sidebar_idx >= 0 &&
          app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
        target_dir = app.sidebar_locations[app.context_menu_sidebar_idx].path;
      }
    } else if (app.context_menu_file_idx >= 0 &&
        app.context_menu_file_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real_idx = app.cur_tab().visible_entries[app.context_menu_file_idx];
      if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()) &&
          app.cur_tab().entries[real_idx].is_dir) {
        target_dir = app.cur_tab().entries[real_idx].path;
      }
    }
    if (!target_dir.empty()) {
      int idx = static_cast<int>(app.tabs.size());
      app.tabs.emplace_back();
      app.tabs[idx].current_path = target_dir;
      app.active_tab = idx;
      navigate_to(app, target_dir);
    }
    draw(app);
    return;
  }

  // ── Tab context menu actions ──
  if (app.context_menu_file_idx == -4) {
    if (action == AppState::ContextMenuAction::ReopenClosedTab) {
      reopen_last_closed_tab(app);
      draw(app);
      return;
    }
    if (action == AppState::ContextMenuAction::CloseTab) {
      if (app.context_menu_tab_idx >= 0 &&
          app.context_menu_tab_idx < static_cast<int>(app.tabs.size())) {
        app.active_tab = app.context_menu_tab_idx;
        close_tab(app);
      }
      draw(app);
      return;
    }
    if (action == AppState::ContextMenuAction::CloseOtherTabs) {
      if (app.context_menu_tab_idx >= 0 &&
          app.context_menu_tab_idx < static_cast<int>(app.tabs.size())) {
        Tab kept = std::move(app.tabs[app.context_menu_tab_idx]);
        app.tabs.clear();
        app.tabs.push_back(std::move(kept));
        app.active_tab = 0;
        app.active_pane = 0;
        reload_dir(app);
        sync_split_panes(app);
      }
      draw(app);
      return;
    }
    if (action == AppState::ContextMenuAction::CloseAllTabs) {
      app.tabs.clear();
      app.tabs.emplace_back();
      app.tabs[0].current_path = home_dir();
      app.active_tab = 0;
      app.active_pane = 0;
      reload_dir(app);
      sync_split_panes(app);
      draw(app);
      return;
    }
    if (action == AppState::ContextMenuAction::DuplicateTab) {
      if (app.context_menu_tab_idx >= 0 &&
          app.context_menu_tab_idx < static_cast<int>(app.tabs.size())) {
        auto src_path = app.tabs[app.context_menu_tab_idx].current_path;
        auto src_view = app.tabs[app.context_menu_tab_idx].view_mode;
        auto src_sort = app.tabs[app.context_menu_tab_idx].sort_field;
        auto src_desc = app.tabs[app.context_menu_tab_idx].sort_descending;
        int idx = static_cast<int>(app.tabs.size());
        app.tabs.emplace_back();
        auto& dup = app.tabs.back();
        dup.current_path = src_path;
        dup.view_mode = src_view;
        dup.sort_field = src_sort;
        dup.sort_descending = src_desc;
        dup.group_by_type = app.tabs[app.context_menu_tab_idx].group_by_type;
        dup.group_field = app.tabs[app.context_menu_tab_idx].group_field;
        app.active_tab = idx;
        navigate_to(app, dup.current_path);
      }
      draw(app);
      return;
    }
    if (action == AppState::ContextMenuAction::ToggleSplitView) {
      if (!app.split_view) {
        // Split at the right-clicked tab (falls back to the active tab)
        enter_split_view(app, app.context_menu_tab_idx);
      } else {
        exit_split_view(app);
      }
      draw(app);
      return;
    }
    if (action == AppState::ContextMenuAction::OpenInNewWindow) {
      if (app.context_menu_tab_idx >= 0 &&
          app.context_menu_tab_idx < static_cast<int>(app.tabs.size())) {
        std::string target_dir = app.tabs[app.context_menu_tab_idx].current_path;
        pid_t pid = fork();
        if (pid == 0) {
          execl("/proc/self/exe", "horizon-files", target_dir.c_str(), nullptr);
          _exit(1);
        }
        // Move tab to new window: remove it from this window
        int idx = app.context_menu_tab_idx;
        app.tabs.erase(app.tabs.begin() + idx);
        if (app.tabs.empty()) {
          app.tabs.emplace_back();
          app.tabs[0].current_path = home_dir();
        }
        if (idx >= static_cast<int>(app.tabs.size()))
          app.active_tab = static_cast<int>(app.tabs.size()) - 1;
        else
          app.active_tab = idx;
        app.active_pane = 0;
        reload_dir(app);
        sync_split_panes(app);
      }
      draw(app);
      return;
    }
    draw(app);
    return;
  }

  // ── Open in new window ──
  if (action == AppState::ContextMenuAction::OpenInNewWindow) {
    std::string target_dir;
    if (app.context_menu_file_idx == -2) {
      if (app.context_menu_sidebar_idx >= 0 &&
          app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
        target_dir = app.sidebar_locations[app.context_menu_sidebar_idx].path;
      }
    } else if (app.context_menu_file_idx >= 0 &&
        app.context_menu_file_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real_idx = app.cur_tab().visible_entries[app.context_menu_file_idx];
      if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()) &&
          app.cur_tab().entries[real_idx].is_dir) {
        target_dir = app.cur_tab().entries[real_idx].path;
      }
    }
    if (!target_dir.empty()) {
      pid_t pid = fork();
      if (pid == 0) {
        execl("/proc/self/exe", "horizon-files", target_dir.c_str(), nullptr);
        _exit(1);
      }
    }
    draw(app);
    return;
  }

  // ── Open as Administrator ──
  if (action == AppState::ContextMenuAction::OpenAsAdmin) {
    std::string target_dir = app.cur_tab().current_path;
    if (app.context_menu_file_idx >= 0 &&
        app.context_menu_file_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real_idx = app.cur_tab().visible_entries[app.context_menu_file_idx];
      if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()) &&
          app.cur_tab().entries[real_idx].is_dir) {
        target_dir = app.cur_tab().entries[real_idx].path;
      }
    }
    open_as_admin(target_dir);
    draw(app);
    return;
  }

  // ── Open file location (navigate to parent dir) ──
  if (action == AppState::ContextMenuAction::OpenFileLocation) {
    if (app.context_menu_file_idx >= 0 &&
        app.context_menu_file_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real_idx = app.cur_tab().visible_entries[app.context_menu_file_idx];
      if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size())) {
        std::string file_path = app.cur_tab().entries[real_idx].path;
        auto slash = file_path.rfind('/');
        if (slash != std::string::npos) {
          std::string parent = file_path.substr(0, slash);
          navigate_to(app, parent);
        }
      }
    }
    draw(app);
    return;
  }

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
      app.compress_hover_format = -1;
      app.compress_hover_level = -1;
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
