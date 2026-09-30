// context_actions_regions.cpp — Exported from features/context_actions.cpp as part of the Step 6 file split.

#include "../../app.hpp"
#include "app/file_browser/features/compare/compare.hpp"
#include "app/file_browser/features/compress/compress.hpp"
#include "app/file_browser/features/dirprops/dirprops.hpp"
#include "app/file_browser/features/progress/progress.hpp"
#include "app/file_browser/features/selection/selection.hpp"
#include "app/file_browser/features/tab_history/tab_history.hpp"
#include "app/file_browser/features/tags/tags.hpp"
#include "app/file_browser/features/view_zoom/view_zoom.hpp"
#include "app/file_browser/features/recursive_search_worker/recursive_search_worker.hpp"

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
#include "base/thread/thread_pool.hpp"
#include "platform/common/palette/matugen_palette.hpp"
#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "dialog/file_chooser_dialog.hpp"
#include "platform/widgets/app_drawer/list/desktop_list.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;
using menu_clock = std::chrono::steady_clock;


namespace eh::file_browser {

// Local-only actions refuse remote targets with a toast (defined below,
// used by earlier handlers).
static bool remote_refused_here(AppState& app, const std::string& path);

// (was `static` in context_actions.cpp)
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

// (was `static` in context_actions.cpp)
// Launches `script` detached (double-fork, no zombie) with the documented
// script environment contract; selection is also passed as argv and
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

// (was `static` in context_actions.cpp)


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

bool ctx_path_edit_menu(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
      return true;
    }
  }
  return false;
}

bool ctx_dialog_text_menu(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
      return true;
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
      return true;
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
      return true;
    }
    if (action == AppState::ContextMenuAction::SelectAll) {
      *sel_start = 0;
      *sel_end = static_cast<int>(buf->size());
      *cursor = *sel_end;
      draw(app);
      return true;
    }
  }
  return false;
}

bool ctx_new_folder(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::NewFolder) {
    app.create_dialog_open = true;
    app.create_is_folder = true;
    app.create_buf = "New Folder";
    app.create_cursor_pos = static_cast<int>(app.create_buf.size());
    app.create_hover_btn = -1;
    draw(app);
    return true;
  }
  return false;
}

bool ctx_new_document(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::NewDocument) {
    app.create_dialog_open = true;
    app.create_is_folder = false;
    app.create_buf = "New Document";
    app.create_cursor_pos = static_cast<int>(app.create_buf.size());
    app.create_hover_btn = -1;
    draw(app);
    return true;
  }
  return false;
}

bool ctx_new_from_template(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
    return true;
  }
  return false;
}

bool ctx_run_script(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::RunScript) {
    run_nemo_script(app, app.context_menu_items[item_idx].data);
    draw(app);
    return true;
  }
  return false;
}

bool ctx_service_run(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::ServiceRun) {
    if (item_idx >= 0 &&
        item_idx < static_cast<int>(app.context_menu_items.size())) {
      const std::string& cmd = app.context_menu_items[item_idx].data;
      if (!cmd.empty()) {
        xdg::spawn_sh_lc_detached(cmd);
        app.operation_status =
            "Running " + app.context_menu_items[item_idx].label + "…";
        app.operation_status_expires_ms = menu_expiry_3s();
      }
    }
    draw(app);
    return true;
  }
  return false;
}

bool ctx_tag_toggle(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action != AppState::ContextMenuAction::TagToggle) return false;
  if (item_idx < 0 ||
      item_idx >= static_cast<int>(app.context_menu_items.size()))
    return true;
  const std::string tag = app.context_menu_items[item_idx].data;
  if (tag.empty()) return true;
  // Targets: multi-selection, else the right-clicked entry. Local paths
  // only (xattrs don't travel over SFTP/Drive; the menu hides otherwise).
  std::vector<std::string> targets;
  bool is_multi = app.cur_tab().multi_selected.size() > 1;
  if (is_multi) {
    for (int vis_idx : app.cur_tab().multi_selected) {
      if (vis_idx < 0 ||
          vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size()))
        continue;
      int r = app.cur_tab().visible_entries[vis_idx];
      if (r < 0 || r >= static_cast<int>(app.cur_tab().entries.size()))
        continue;
      const std::string& p = app.cur_tab().entries[r].path;
      if (!p.empty() && p[0] == '/') targets.push_back(p);
    }
  } else if (app.context_menu_file_idx >= 0 &&
             app.context_menu_file_idx <
                 static_cast<int>(app.cur_tab().visible_entries.size())) {
    int r = app.cur_tab().visible_entries[app.context_menu_file_idx];
    if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size())) {
      const std::string& p = app.cur_tab().entries[r].path;
      if (!p.empty() && p[0] == '/') targets.push_back(p);
    }
  }
  int changed = 0;
  for (auto& t : targets) {
    if (write_xdg_tags(t, toggle_tag(read_xdg_tags(t), tag))) ++changed;
  }
  if (changed > 0) {
    app.operation_status =
        (has_tag(read_xdg_tags(targets.front()), tag) ? "Tagged " : "Untagged ") + tag;
    app.operation_status_expires_ms = menu_expiry_3s();
    reload_dir(app);
  }
  draw(app);
  return true;
}

bool ctx_reload(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::Reload) {
    reload_dir(app);
    draw(app);
    return true;
  }
  return false;
}

bool ctx_copy_location(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::CopyLocation) {
    app.clipboard.copy_text(app.cur_tab().current_path);
    draw(app);
    return true;
  }
  return false;
}

bool ctx_select_all(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::SelectAll) {
    app.cur_tab().multi_selected.clear();
    for (int vi = 0; vi < static_cast<int>(app.cur_tab().visible_entries.size()); ++vi)
      app.cur_tab().multi_selected.push_back(vi);
    draw(app);
    return true;
  }
  return false;
}

bool ctx_open_with(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::OpenWith) {
    int sel = app.cur_tab().selected_idx;
    if (sel >= 0 && sel < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real = app.cur_tab().visible_entries[sel];
      if (real >= 0 && real < static_cast<int>(app.cur_tab().entries.size()))
        open_with_open(app, app.cur_tab().entries[real].path);
    }
    draw(app);
    return true;
  }
  return false;
}

bool ctx_properties(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::Properties &&
      (app.context_menu_file_idx == -1 || app.context_menu_file_idx == -5)) {
    show_properties(app, app.cur_tab().current_path);
    draw(app);
    return true;
  }
  return false;
}

bool ctx_paste(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::Paste) {
    paste_clipboard(app);
    return true;
  }
  return false;
}

bool ctx_open_in_terminal(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
    if (remote_refused_here(app, target_dir)) return true;
    open_terminal_at(app, target_dir);
    return true;
  }
  return false;
}

bool ctx_remove_from_favorites(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
    return true;
  }
  return false;
}

bool ctx_unmount_drive(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::UnmountDrive) {
    if (app.context_menu_sidebar_idx >= 0 &&
        app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
      unmount_drive(app, app.context_menu_sidebar_idx);
    }
    draw(app);
    return true;
  }
  return false;
}

bool ctx_mount_drive(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::MountDrive) {
    if (app.context_menu_sidebar_idx >= 0 &&
        app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
      mount_drive(app, app.context_menu_sidebar_idx);
    }
    draw(app);
    return true;
  }
  return false;
}

// Fire-and-forget trash auto-maintenance. Snapshots settings (they may
// change mid-flight); zero-cost when disabled. ThreadPool mirrors the
// copy engine's fire-and-forget pattern.
void schedule_trash_maintain(AppState& app) {
  if (!app.trash_auto_purge) return;
  int days = app.trash_max_days;
  uint64_t max_bytes =
      static_cast<uint64_t>(app.trash_max_mb) * 1024ULL * 1024ULL;
  ThreadPool::instance().enqueue([days, max_bytes] {
    xdg::trash_maintain(days, max_bytes);
  });
}

bool ctx_disk_usage(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action != AppState::ContextMenuAction::DiskUsage) return false;
  if (app.context_menu_sidebar_idx >= 0 &&
      app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
    const auto& loc = app.sidebar_locations[app.context_menu_sidebar_idx];
    // Local disks (Kind::Drive) plus the root filesystem row (Kind::Root,
    // always "/") — both are scannable local paths. Unmounted Drive rows
    // point at /dev nodes, never a directory, so require mounted there.
    bool usable =
        !loc.path.empty() && loc.path[0] == '/' &&
        (loc.kind == SidebarLocation::Kind::Root ||
         (loc.kind == SidebarLocation::Kind::Drive && loc.is_mounted));
    if (usable) {
      open_disk_usage(app, loc.path);
      draw(app);
      return true;
    }
  }
  // File-row folders from the main view (and tree rows): local absolute
  // directories only; stat re-verifies (dead links, virtual rows).
  {
    std::string fpath;
    if (app.context_menu_file_idx == -9) {
      fpath = app.context_menu_tree_entry.path;
    } else if (app.context_menu_file_idx >= 0 &&
               app.context_menu_file_idx <
                   static_cast<int>(app.cur_tab().visible_entries.size())) {
      int real_idx = app.cur_tab().visible_entries[app.context_menu_file_idx];
      if (real_idx >= 0 &&
          real_idx < static_cast<int>(app.cur_tab().entries.size()) &&
          app.cur_tab().entries[real_idx].is_dir)
        fpath = app.cur_tab().entries[real_idx].path;
    }
    if (!fpath.empty() && fpath[0] == '/') {
      struct stat st {};
      if (::stat(fpath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        open_disk_usage(app, fpath);
        draw(app);
        return true;
      }
    }
  }
  (void)item_idx;
  return false;
}

bool ctx_open(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::Open &&
      app.context_menu_sidebar_idx >= 0 &&
      app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
    auto& loc = app.sidebar_locations[app.context_menu_sidebar_idx];
    if (loc.kind == SidebarLocation::Kind::Trash) {
      app.cur_tab().current_path = loc.path;
      navigate_to(app, loc.path);
      draw(app);
      return true;
    }
    if (loc.kind == SidebarLocation::Kind::Recent ||
        loc.kind == SidebarLocation::Kind::Starred ||
        loc.kind == SidebarLocation::Kind::Remote ||
        loc.kind == SidebarLocation::Kind::Computer ||
        loc.kind == SidebarLocation::Kind::Drive ||
        loc.kind == SidebarLocation::Kind::Home ||
        loc.kind == SidebarLocation::Kind::Favorite) {
      navigate_to(app, loc.path);
      draw(app);
      return true;
    }
  }
  return false;
}

bool ctx_connect_server(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  (void)item_idx;
  if (action == AppState::ContextMenuAction::ConnectServer) {
    open_connect_dialog(app);
    draw(app);
    return true;
  }
  return false;
}

bool ctx_remove_server(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  (void)item_idx;
  if (action == AppState::ContextMenuAction::RemoveServer) {
    if (app.context_menu_sidebar_idx >= 0 &&
        app.context_menu_sidebar_idx < static_cast<int>(app.sidebar_locations.size())) {
      const std::string& uri = app.sidebar_locations[app.context_menu_sidebar_idx].path;
      auto it = std::remove_if(app.remote_servers.begin(), app.remote_servers.end(),
                               [&](const AppState::RemoteServer& s) {
                                 return build_sftp_uri(s.host, s.user, s.port, s.path) == uri;
                               });
      if (it != app.remote_servers.end()) {
        app.remote_servers.erase(it, app.remote_servers.end());
        save_file_browser_settings(app);
        refresh_sidebar(app);
        app.operation_status = "Server removed";
        app.operation_status_expires_ms = menu_expiry_3s();
      }
    }
    draw(app);
    return true;
  }
  return false;
}

bool ctx_empty_trash(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
    return true;
  }
  return false;
}

bool ctx_clear_recent(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::ClearRecent) {
    app.recent.clear();
    save_file_browser_settings(app);
    if (app.cur_tab().current_path == "recent://") reload_dir(app);
    app.operation_status = "Recent cleared";
    app.operation_status_expires_ms = menu_expiry_3s();
    draw(app);
    return true;
  }
  return false;
}

bool ctx_crumb_nav(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::BreadcrumbNav) {
    if (item_idx >= 0 &&
        item_idx < static_cast<int>(app.context_menu_items.size())) {
      const std::string& target = app.context_menu_items[item_idx].data;
      if (!target.empty()) {
        std::error_code ec;
        if (fs::is_directory(target, ec) && !ec) navigate_to(app, target);
      }
    }
    draw(app);
    return true;
  }
  return false;
}

bool ctx_add_to_favorites(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
    return true;
  }
  return false;
}

// Default startup folder: right-click any local folder (or My Computer /
// Recent / Starred) to set it as the folder the file manager opens to
// (persisted in the file-browser TOML), right-click again to unset.
// Anything else gets a toast instead.
bool startup_folder_ok(const std::string& p) {
  if (p == "computer://" || p == "recent://" || p == "starred://") return true;
  if (p.empty() || p[0] != '/') return false;
  std::error_code ec;
  return fs::is_directory(p, ec) && !ec;
}

namespace {

// Resolve the folder targeted by the current context menu: a folder row
// (or tree row), a sidebar location, or — for background menus — the
// current folder. Empty when the menu targets nothing folder-like.
std::string startup_menu_target(AppState& app) {
  if (app.context_menu_file_idx == -9) {
    return app.context_menu_tree_entry.path;
  }
  if (app.context_menu_file_idx >= 0 &&
      app.context_menu_file_idx <
          static_cast<int>(app.cur_tab().visible_entries.size())) {
    int r = app.cur_tab().visible_entries[app.context_menu_file_idx];
    if (r >= 0 && r < static_cast<int>(app.cur_tab().entries.size()) &&
        app.cur_tab().entries[r].is_dir)
      return app.cur_tab().entries[r].path;
    return {};
  }
  if (app.context_menu_file_idx == -2 &&
      app.context_menu_sidebar_idx >= 0 &&
      app.context_menu_sidebar_idx <
          static_cast<int>(app.sidebar_locations.size())) {
    return app.sidebar_locations[app.context_menu_sidebar_idx].path;
  }
  // Background menu: the folder being viewed.
  return app.cur_tab().current_path;
}
} // namespace

bool ctx_startup_folder(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  (void)item_idx;
  if (action == AppState::ContextMenuAction::SetStartupFolder) {
    std::string target = startup_menu_target(app);
    if (!startup_folder_ok(target)) {
      app.operation_status = "That location can't be the default folder";
      app.operation_status_expires_ms = menu_expiry_3s();
      draw(app);
      return true;
    }
    app.startup_folder = target;
    save_file_browser_settings(app);
    app.operation_status = "Default folder set to " + target;
    app.operation_status_expires_ms = menu_expiry_3s();
    draw(app);
    return true;
  }
  if (action == AppState::ContextMenuAction::ClearStartupFolder) {
    app.startup_folder.clear();
    save_file_browser_settings(app);
    app.operation_status = "Default folder cleared (opens to home)";
    app.operation_status_expires_ms = menu_expiry_3s();
    draw(app);
    return true;
  }
  return false;
}

bool ctx_settings(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (action == AppState::ContextMenuAction::Settings) {
    open_settings(app);
    draw(app);
    return true;
  }
  return false;
}

// loop in draw_top_bar() (narrow windows). The ⋮ menu
// appends these entries only while their button is hidden, so nothing is
// ever unreachable.
bool ctx_toolbar_overflow(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  auto& o_search_active = app.active_pane ? app.r_search_active : app.search_active;
  auto& o_recursive = app.active_pane ? app.r_recursive_search_active : app.recursive_search_active;
  auto& o_query = app.active_pane ? app.r_search_query : app.search_query;
  auto& o_rquery = app.active_pane ? app.r_recursive_search_query : app.recursive_search_query;
  auto& o_cursor = app.active_pane ? app.r_search_cursor : app.search_cursor;
  auto& o_sel_start = app.active_pane ? app.r_search_sel_start : app.search_sel_start;
  auto& o_sel_end = app.active_pane ? app.r_search_sel_end : app.search_sel_end;

  if (action == AppState::ContextMenuAction::ToolbarSearchFolder) {
    if (o_search_active) {
      reset_search_filters(app);
      o_search_active = false;
      o_recursive = false;
      o_query.clear();
      o_rquery.clear();
      recursive_search_worker().cancel();
      reload_dir(app);
    } else {
      o_recursive = false;
      o_search_active = true;
      o_query.clear();
      o_rquery.clear();
      recursive_search_worker().cancel();
      o_cursor = 0;
      o_sel_start = -1;
      o_sel_end = -1;
    }
    (app.active_pane ? app.r_path_editing : app.path_editing) = false;
    draw(app);
    return true;
  }
  if (action == AppState::ContextMenuAction::ToolbarSearchHome) {
    if (o_recursive) {
      reset_search_filters(app);
      o_recursive = false;
      o_search_active = false;
      o_query.clear();
      o_rquery.clear();
      recursive_search_worker().cancel();
      reload_dir(app);
    } else {
      o_search_active = false;
      o_recursive = true;
      o_query.clear();
      o_rquery.clear();
      recursive_search_worker().cancel();
      o_cursor = 0;
      o_sel_start = -1;
      o_sel_end = -1;
    }
    (app.active_pane ? app.r_path_editing : app.path_editing) = false;
    draw(app);
    return true;
  }
  if (action == AppState::ContextMenuAction::ToolbarCycleView) {
    auto cur = app.cur_tab().view_mode;
    if (cur == ViewMode::List) app.cur_tab().view_mode = ViewMode::Grid;
    else if (cur == ViewMode::Grid) app.cur_tab().view_mode = ViewMode::Compact;
    else if (cur == ViewMode::Compact) app.cur_tab().view_mode = ViewMode::Tree;
    else app.cur_tab().view_mode = ViewMode::List;
    app.last_browser_view_mode = app.cur_tab().view_mode;
    save_file_browser_settings(app);
    draw(app);
    return true;
  }
  if (action == AppState::ContextMenuAction::ToolbarSortMenu) {
    bool was_open = app.active_pane ? app.r_sort_menu_open : app.sort_menu_open;
    app.r_sort_menu_open = false;
    app.sort_menu_open = false;
    if (!was_open)
      (app.active_pane ? app.r_sort_menu_open : app.sort_menu_open) = true;
    (app.active_pane ? app.r_sort_menu_hover : app.sort_menu_hover) = -1;
    (app.active_pane ? app.r_sort_menu_scroll : app.sort_menu_scroll) = 0;
    draw(app);
    return true;
  }
  return false;
}

bool ctx_open_in_new_tab(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
    return true;
  }
  return false;
}

bool ctx_tab_menu(AppState& app, int item_idx, AppState::ContextMenuAction action) {
  if (app.context_menu_file_idx == -4) {
    if (action == AppState::ContextMenuAction::ReopenClosedTab) {
      reopen_last_closed_tab(app);
      draw(app);
      return true;
    }
    if (action == AppState::ContextMenuAction::CloseTab) {
      if (app.context_menu_tab_idx >= 0 &&
          app.context_menu_tab_idx < static_cast<int>(app.tabs.size())) {
        app.active_tab = app.context_menu_tab_idx;
        close_tab(app);
      }
      draw(app);
      return true;
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
      return true;
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
      return true;
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
      return true;
    }
    if (action == AppState::ContextMenuAction::ToggleSplitView) {
      if (!app.split_view) {
        // Split at the right-clicked tab (falls back to the active tab)
        enter_split_view(app, app.context_menu_tab_idx);
      } else {
        exit_split_view(app);
      }
      draw(app);
      return true;
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
      return true;
    }
    draw(app);
    return true;
  }
  return false;
}

bool ctx_open_in_new_window(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
    return true;
  }
  return false;
}

static bool remote_refused_here(AppState& app, const std::string& path) {
  if (is_remote_uri(path)) {
    app.operation_status = "Not available for remote files yet";
    app.operation_status_expires_ms = menu_expiry_3s();
    draw(app);
    return true;
  }
  return false;
}

bool ctx_open_as_admin(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
    if (remote_refused_here(app, target_dir)) return true;
    open_as_admin(target_dir);
    draw(app);
    return true;
  }
  return false;
}

bool ctx_open_file_location(AppState& app, int item_idx, AppState::ContextMenuAction action) {
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
    return true;
  }
  return false;
}

} // namespace eh::file_browser
