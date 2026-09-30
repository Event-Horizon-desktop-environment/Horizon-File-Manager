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
#include "services/udisks2/udisks2_drive_service.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;
using menu_clock = std::chrono::steady_clock;

namespace eh::file_browser {


static std::string expand_home_path(std::string p) {
  const char* home = std::getenv("HOME");
  if (!home || !home[0]) return p;
  const std::string h = home;
  if (p.compare(0, 2, "~/") == 0) p = h + p.substr(1);
  for (std::size_t pos = p.find("$HOME"); pos != std::string::npos;
       pos = p.find("$HOME", pos + h.size()))
    p.replace(pos, 5, h);
  return p;
}

static std::string templates_dir() {
  const char* home = std::getenv("HOME");
  if (!home || !home[0]) return {};
  std::ifstream in(std::string(home) + "/.config/user-dirs.dirs");
  if (in) {
    std::string line;
    while (std::getline(in, line)) {
      auto hash = line.find('#');
      if (hash != std::string::npos) line.resize(hash);
      const std::string key = "XDG_TEMPLATES_DIR";
      auto kpos = line.find(key);
      if (kpos == std::string::npos) continue;
      auto eq = line.find('=', kpos + key.size());
      if (eq == std::string::npos) continue;
      std::string val = line.substr(eq + 1);
      auto notsp = [](unsigned char c) { return !std::isspace(c); };
      val.erase(val.begin(), std::find_if(val.begin(), val.end(), notsp));
      val.erase(std::find_if(val.rbegin(), val.rend(), notsp).base(), val.end());
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
        val = val.substr(1, val.size() - 2);
      if (!val.empty()) return expand_home_path(val);
    }
  }
  return std::string(home) + "/Templates";
}

// Depth-limited recursive scan; directories become nested submenu items.
static void collect_templates(const fs::path& dir, int depth,
                              std::vector<AppState::ContextMenuItem>& out) {
  std::error_code ec;
  fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
  if (ec) return;
  std::vector<fs::directory_entry> entries;
  for (; it != end && !ec; it.increment(ec))
    entries.push_back(*it);
  std::sort(entries.begin(), entries.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
              return a.path().filename().string() < b.path().filename().string();
            });
  for (const auto& e : entries) {
    std::string name = e.path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    std::error_code ec2;
    bool is_dir = e.is_directory(ec2);
    if (is_dir) {
      if (depth >= 3) continue;
      std::vector<AppState::ContextMenuItem> sub;
      collect_templates(e.path(), depth + 1, sub);
      if (sub.empty()) continue;
      AppState::ContextMenuItem hdr =
          AppState::menu_item(AppState::ContextMenuAction::Separator, name);
      hdr.sub_items = std::move(sub);
      out.push_back(std::move(hdr));
    } else if (!ec2) {
      out.push_back(AppState::menu_item(AppState::ContextMenuAction::NewFromTemplate,
                                        name, e.path().string()));
    }
  }
}

// Builds a "New From Template" submenu item; returns false when no templates.
static bool build_template_submenu(AppState::ContextMenuItem& out_item) {
  std::vector<AppState::ContextMenuItem> items;
  collect_templates(fs::path(templates_dir()), 0, items);
  if (items.empty()) return false;
  out_item.action = AppState::ContextMenuAction::Separator;
  out_item.label = "New From Template";
  out_item.sub_items = std::move(items);
  return true;
}

void insert_template_submenu(AppState& app, std::size_t pos) {
  AppState::ContextMenuItem item;
  if (!build_template_submenu(item)) return;
  if (pos > app.context_menu_items.size())
    pos = app.context_menu_items.size();
  app.context_menu_items.insert(app.context_menu_items.begin() + pos,
                                std::move(item));
}


static std::string scripts_dir() {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  std::string base;
  if (xdg && xdg[0]) {
    base = xdg;
  } else {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return {};
    base = std::string(home) + "/.local/share";
  }
  return base + "/nemo/scripts";
}

static void collect_scripts(const fs::path& dir, int depth,
                            std::vector<AppState::ContextMenuItem>& out) {
  std::error_code ec;
  fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
  if (ec) return;
  std::vector<fs::directory_entry> entries;
  for (; it != end && !ec; it.increment(ec))
    entries.push_back(*it);
  std::sort(entries.begin(), entries.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
              return a.path().filename().string() < b.path().filename().string();
            });
  for (const auto& e : entries) {
    std::string name = e.path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    std::error_code ec2;
    bool is_dir = e.is_directory(ec2);
    if (is_dir) {
      if (depth >= 3) continue;
      std::vector<AppState::ContextMenuItem> sub;
      collect_scripts(e.path(), depth + 1, sub);
      if (sub.empty()) continue;
      AppState::ContextMenuItem hdr =
          AppState::menu_item(AppState::ContextMenuAction::Separator, name);
      hdr.sub_items = std::move(sub);
      out.push_back(std::move(hdr));
    } else if (!ec2 && ::access(e.path().c_str(), X_OK) == 0) {
      out.push_back(AppState::menu_item(AppState::ContextMenuAction::RunScript,
                                        name, e.path().string()));
    }
  }
}

void insert_scripts_submenu(AppState& app, std::size_t pos) {
  std::vector<AppState::ContextMenuItem> items;
  collect_scripts(fs::path(scripts_dir()), 0, items);
  if (items.empty()) return;
  AppState::ContextMenuItem item;
  item.action = AppState::ContextMenuAction::Separator;
  item.label = "Scripts";
  item.sub_items = std::move(items);
  if (pos > app.context_menu_items.size())
    pos = app.context_menu_items.size();
  app.context_menu_items.insert(app.context_menu_items.begin() + pos,
                                std::move(item));
}

// Minimal subset: Type=Service (or absent), Actions + [Desktop Action X]
// groups, MimeType filtering, Exec with %f %F %u %U %d %D %n %N %c codes.
// The expanded shell command is stored in the item's data payload and run
// detached on click (same trust model as the Scripts submenu).

namespace {

struct ServiceTarget {
  std::string path;
  std::string mime;
  bool is_dir = false;
};

std::string svc_trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

// "a;b;c;" -> {"a","b","c"}
std::vector<std::string> svc_split_semi(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t end = s.find(';', start);
    std::string part = svc_trim(end == std::string::npos ? s.substr(start)
                                                         : s.substr(start, end - start));
    if (!part.empty()) out.push_back(part);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

bool svc_mime_matches(const std::vector<std::string>& patterns,
                      const std::string& mime) {
  if (patterns.empty()) return true; // no constraint: applies everywhere
  auto slash = mime.find('/');
  std::string top = slash == std::string::npos ? mime : mime.substr(0, slash);
  for (const auto& p : patterns) {
    if (p == mime || p == "all/all") return true;
    if (p.size() > 2 && p.compare(p.size() - 2, 2, "/*") == 0 &&
        p.substr(0, p.size() - 2) == top)
      return true;
  }
  return false;
}

std::string svc_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

std::string svc_file_uri(const std::string& p) {
  std::string uri = "file://";
  for (unsigned char c : p) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      uri += static_cast<char>(c);
    } else {
      char hex[4];
      snprintf(hex, sizeof hex, "%%%02X", c);
      uri += hex;
    }
  }
  return uri;
}

bool svc_exe_ok(const std::string& prog) {
  if (prog.empty()) return true;
  if (prog[0] == '/') return ::access(prog.c_str(), X_OK) == 0;
  if (const char* pe = std::getenv("PATH")) {
    std::string paths(pe);
    size_t start = 0;
    while (start <= paths.size()) {
      size_t end = paths.find(':', start);
      std::string dir = end == std::string::npos ? paths.substr(start)
                                                 : paths.substr(start, end - start);
      if (!dir.empty() && ::access((dir + "/" + prog).c_str(), X_OK) == 0)
        return true;
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  return false;
}

std::vector<std::string> svc_menu_dirs() {
  std::vector<std::string> dirs;
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0])
    dirs.push_back(std::string(xdg) + "/kio/servicemenus");
  else if (const char* home = std::getenv("HOME"); home && home[0])
    dirs.push_back(std::string(home) + "/.local/share/kio/servicemenus");
  if (const char* xdg = std::getenv("XDG_DATA_DIRS"); xdg && xdg[0]) {
    std::string all(xdg);
    size_t start = 0;
    while (start <= all.size()) {
      size_t end = all.find(':', start);
      std::string d = end == std::string::npos ? all.substr(start)
                                               : all.substr(start, end - start);
      if (!d.empty()) dirs.push_back(d + "/kio/servicemenus");
      if (end == std::string::npos) break;
      start = end + 1;
    }
  } else {
    dirs.emplace_back("/usr/local/share/kio/servicemenus");
    dirs.emplace_back("/usr/share/kio/servicemenus");
  }
  return dirs;
}

// Expand Exec field codes against the target list. Returns empty when the
// entry is unusable (missing Exec). Appends the file list when Exec names
// no file (spec behavior).
std::string svc_expand_exec(const std::string& exec,
                            const std::vector<ServiceTarget>& targets) {
  std::string files, urls, dirs, names;
  for (size_t i = 0; i < targets.size(); ++i) {
    const auto& t = targets[i];
    std::string q = svc_quote(t.path);
    std::string u = svc_quote(svc_file_uri(t.path));
    std::string base = fs::path(t.path).filename().string();
    files += (i ? " " : "") + q;
    urls += (i ? " " : "") + u;
    if (t.is_dir) dirs += (dirs.empty() ? "" : " ") + q;
    names += (i ? " " : "") + svc_quote(base);
  }
  if (dirs.empty() && !targets.empty())
    dirs = svc_quote(fs::path(targets[0].path).parent_path().string());
  std::string first_q = targets.empty() ? "''" : svc_quote(targets[0].path);
  std::string first_u =
      targets.empty() ? "''" : svc_quote(svc_file_uri(targets[0].path));
  std::string first_dir = dirs.find(' ') == std::string::npos
                              ? dirs
                              : dirs.substr(0, dirs.find(' '));
  std::string first_name = targets.empty()
                               ? "''"
                               : svc_quote(fs::path(targets[0].path).filename().string());

  std::string out;
  bool used_file = false;
  for (size_t i = 0; i < exec.size(); ++i) {
    if (exec[i] == '%' && i + 1 < exec.size()) {
      char c = exec[i + 1];
      if (c == '%') {
        out += '%';
        ++i;
        continue;
      }
      // A '%' not followed by a letter is literal (e.g. "50%"): keep it
      // without swallowing the next character.
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        out += '%';
        continue;
      }
      switch (c) {
        case 'f': out += first_q; used_file = true; break;
        case 'F': out += files; used_file = true; break;
        case 'u': out += first_u; used_file = true; break;
        case 'U': out += urls; used_file = true; break;
        case 'd': out += first_dir; used_file = true; break;
        case 'D': out += dirs; used_file = true; break;
        case 'n': out += first_name; used_file = true; break;
        case 'N': out += names; used_file = true; break;
        case 'c': out += std::to_string(targets.size()); break;
        case '%': out += '%'; break;
        default: break; // drop unknown codes (%i %k %m %v …)
      }
      ++i;
    } else {
      out += exec[i];
    }
  }
  if (!used_file && !files.empty()) {
    out += " ";
    out += files;
  }
  return svc_trim(out);
}

void collect_service_items(const std::vector<ServiceTarget>& targets,
                           std::vector<AppState::ContextMenuItem>& out) {
  if (targets.empty() || out.size() >= 30) return;
  for (const auto& dir : svc_menu_dirs()) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) continue;
    std::vector<fs::directory_entry> files;
    for (; it != end && !ec; it.increment(ec)) {
      std::error_code ec2;
      if (!it->is_regular_file(ec2) || ec2) continue;
      if (it->path().extension() != ".desktop") continue;
      files.push_back(*it);
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) {
                return a.path().filename().string() < b.path().filename().string();
              });
    for (const auto& e : files) {
      if (out.size() >= 30) return;
      std::ifstream f(e.path());
      if (!f) continue;
      std::map<std::string, std::map<std::string, std::string>> groups;
      std::string cur = "Desktop Entry";
      std::string line;
      while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = svc_trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.size() > 2 && t.front() == '[' && t.back() == ']') {
          cur = t.substr(1, t.size() - 2);
          continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        // Locale-suffixed keys (Name[de]) lose to the untranslated lookup
        // below unless nothing else exists; keep first occurrence simple:
        // prefer exact "Name"/"Exec", accept one suffixed fallback.
        std::string key = svc_trim(t.substr(0, eq));
        std::string val = svc_trim(t.substr(eq + 1));
        auto& g = groups[cur];
        if (g.find(key) == g.end()) g[key] = val;
      }
      const auto& main = groups["Desktop Entry"];
      auto get = [&](const char* k) -> std::string {
        auto it = main.find(k);
        return it == main.end() ? std::string() : it->second;
      };
      std::string type = get("Type");
      if (!type.empty() && type != "Service") continue;
      if (get("NoDisplay") == "true" || get("Hidden") == "true") continue;
      // TryExec gate (Dolphin honors it).
      std::string try_exec = get("TryExec");
      if (!try_exec.empty()) {
        std::string prog = try_exec.substr(0, try_exec.find(' '));
        if (!svc_exe_ok(prog)) continue;
      }
      std::vector<std::string> mimes = svc_split_semi(get("MimeType"));
      bool ok = true;
      for (const auto& t : targets) {
        if (!svc_mime_matches(mimes, t.mime)) {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
      for (const auto& aid : svc_split_semi(get("Actions"))) {
        if (out.size() >= 30) return;
        std::string gname = "Desktop Action " + aid;
        auto git = groups.find(gname);
        if (git == groups.end()) continue;
        auto nget = [&](const char* k) -> std::string {
          auto it = git->second.find(k);
          return it == git->second.end() ? std::string() : it->second;
        };
        std::string aname = nget("Name");
        if (aname.empty()) aname = aid;
        std::string exec = nget("Exec");
        if (exec.empty()) continue;
        std::string cmd = svc_expand_exec(exec, targets);
        if (cmd.empty()) continue;
        // Run from the targets' common directory (Dolphin behavior).
        std::string cwd = fs::path(targets[0].path).parent_path().string();
        if (!cwd.empty()) cmd = "cd " + svc_quote(cwd) + " && " + cmd;
        out.push_back(AppState::menu_item(AppState::ContextMenuAction::ServiceRun,
                                          aname, cmd));
      }
    }
  }
}

} // namespace






static void conflict_cleanup(AppState& app) {
  app.conflict_open = false;
  app.conflict_srcs.clear();
  app.conflict_queue.clear();
  app.conflict_overwrite.clear();
  app.conflict_skipped.clear();
  app.conflict_dst_names.clear();
  app.conflict_dest_dir.clear();
  app.conflict_is_move = false;
  app.conflict_success_toast.clear();
  app.conflict_clear_cut = false;
  app.conflict_apply_all = false;
  app.conflict_hover_btn = -1;
}

// Launch the async copy/move for everything that wasn't skipped.
// Drive traffic goes through the native worker, remote through GIO (no
// undo records in either case); pure-local traffic keeps the engine.
static bool conflict_involves_drive(AppState& app) {
  if (is_drive_uri(app.conflict_dest_dir)) return true;
  for (const auto& s : app.conflict_srcs)
    if (is_drive_uri(s)) return true;
  return false;
}

static void start_planned_drive_operation(AppState& app) {
  std::vector<std::string> final_srcs;
  std::vector<std::string> dst_names;
  for (size_t i = 0; i < app.conflict_srcs.size(); ++i) {
    const auto& s = app.conflict_srcs[i];
    if (std::find(app.conflict_skipped.begin(), app.conflict_skipped.end(), s)
        != app.conflict_skipped.end())
      continue;
    final_srcs.push_back(s);
    dst_names.push_back(i < app.conflict_dst_names.size() ? app.conflict_dst_names[i] : "");
  }
  if (final_srcs.empty()) {
    conflict_cleanup(app);
    return;
  }

  bool is_move = app.conflict_is_move;
  bool clear_cut = app.conflict_clear_cut;
  std::string toast = app.conflict_success_toast;
  auto prog = std::make_shared<OperationProgress>();
  prog->type = is_move ? OperationType::Move : OperationType::Copy;
  auto op_error = std::make_shared<std::string>();
  app.op_progress = prog;
  app.ops_panel_open = true;
  drive_do_copy_move(final_srcs, app.conflict_dest_dir, is_move, prog,
      [&app, is_move, clear_cut, toast, op_error](bool cancelled) {
        if (!cancelled) {
          if (clear_cut) app.cut_paths.clear();
          if (!app.op_progress || app.op_progress->success.load()) {
            app.operation_status = toast;
          } else if (!op_error->empty()) {
            app.operation_status = *op_error;
          } else {
            app.operation_status = is_move ? "Move incomplete" : "Copy incomplete";
          }
          app.operation_status_expires_ms = menu_expiry_3s();
        }
        app.op_progress.reset();
        conflict_cleanup(app);
        reload_dir(app);
        draw(app);
      },
      app.conflict_overwrite, dst_names, op_error);
}

static bool conflict_involves_remote(AppState& app) {
  if (is_remote_uri(app.conflict_dest_dir)) return true;
  for (const auto& s : app.conflict_srcs)
    if (is_remote_uri(s)) return true;
  return false;
}

// Launch the async copy/move for everything that wasn't skipped
static void start_planned_remote_operation(AppState& app) {
  std::vector<std::string> final_srcs;
  std::vector<std::string> dst_names;
  for (size_t i = 0; i < app.conflict_srcs.size(); ++i) {
    const auto& s = app.conflict_srcs[i];
    if (std::find(app.conflict_skipped.begin(), app.conflict_skipped.end(), s)
        != app.conflict_skipped.end())
      continue;
    final_srcs.push_back(s);
    dst_names.push_back(i < app.conflict_dst_names.size() ? app.conflict_dst_names[i] : "");
  }
  if (final_srcs.empty()) {
    conflict_cleanup(app);
    return;
  }

  bool is_move = app.conflict_is_move;
  bool clear_cut = app.conflict_clear_cut;
  std::string toast = app.conflict_success_toast;
  auto prog = std::make_shared<OperationProgress>();
  prog->type = is_move ? OperationType::Move : OperationType::Copy;
  auto op_error = std::make_shared<std::string>();
  app.op_progress = prog;
  app.ops_panel_open = true;
  remote_do_copy_move(final_srcs, app.conflict_dest_dir, is_move, prog,
      [&app, is_move, clear_cut, toast, op_error](bool cancelled) {
        if (!cancelled) {
          if (clear_cut) app.cut_paths.clear();
          if (!app.op_progress || app.op_progress->success.load()) {
            app.operation_status = toast;
          } else if (!op_error->empty()) {
            app.operation_status = *op_error;
          } else {
            app.operation_status = is_move ? "Move incomplete" : "Copy incomplete";
          }
          app.operation_status_expires_ms = menu_expiry_3s();
        }
        app.op_progress.reset();
        conflict_cleanup(app);
        reload_dir(app);
        draw(app);
      },
      app.conflict_overwrite, dst_names, op_error);
}

// Single-file remote copy under a precomputed destination name (used by
// Duplicate). Goes through the planner so conflicts/progress match pastes.
static void start_planned_fs_operation(AppState& app);
void launch_remote_copy_as(AppState& app, const std::string& src,
                           const std::string& dest_dir, const std::string& dst_name,
                           const std::string& toast) {
  conflict_cleanup(app);
  app.conflict_dest_dir = dest_dir;
  app.conflict_is_move = false;
  app.conflict_success_toast = toast;
  app.conflict_clear_cut = false;
  app.conflict_srcs.push_back(src);
  app.conflict_dst_names.push_back(dst_name);
  start_planned_fs_operation(app);
}

static void start_planned_fs_operation(AppState& app) {
  std::vector<std::string> final_srcs;
  std::vector<std::string> dst_names;
  for (size_t i = 0; i < app.conflict_srcs.size(); ++i) {
    const auto& s = app.conflict_srcs[i];
    if (std::find(app.conflict_skipped.begin(), app.conflict_skipped.end(), s)
        != app.conflict_skipped.end())
      continue;
    final_srcs.push_back(s);
    dst_names.push_back(i < app.conflict_dst_names.size() ? app.conflict_dst_names[i] : "");
  }
  if (final_srcs.empty()) {
    conflict_cleanup(app);
    return;
  }

  if (conflict_involves_drive(app)) {
    start_planned_drive_operation(app);
    return;
  }

  if (conflict_involves_remote(app)) {
    start_planned_remote_operation(app);
    return;
  }

  // Undo record with the actual destination paths
  {
    AppState::UndoRecord rec{app.conflict_is_move ? AppState::UndoRecord::Type::PasteCut
                                                  : AppState::UndoRecord::Type::PasteCopy, {}, {}};
    fs::path dest(app.conflict_dest_dir);
    for (size_t i = 0; i < final_srcs.size(); ++i) {
      std::string name = dst_names[i].empty()
          ? fs::path(final_srcs[i]).filename().string()
          : dst_names[i];
      if (app.conflict_is_move) rec.paths_a.push_back(final_srcs[i]);
      rec.paths_b.push_back((dest / name).string());
    }
    app.redo_stack.clear();
    app.undo_stack.push_back(std::move(rec));
    if (app.undo_stack.size() > app.kMaxUndo)
      app.undo_stack.erase(app.undo_stack.begin());
  }

  bool is_move = app.conflict_is_move;
  bool clear_cut = app.conflict_clear_cut;
  std::string toast = app.conflict_success_toast;
  auto prog = std::make_shared<OperationProgress>();
  prog->type = is_move ? OperationType::Move : OperationType::Copy;
  // Assign to the app BEFORE starting: start_async_op may finish
  // synchronously (empty source list), and the completion callback resets
  // op_progress — assigning afterwards would resurrect a finished op and
  // strand the operations panel open.
  app.op_progress = prog;
  app.ops_panel_open = true;
  start_async_op(final_srcs, app.conflict_dest_dir, is_move, prog,
      [&app, is_move, clear_cut, toast](bool cancelled) {
        if (!cancelled) {
          if (clear_cut) app.cut_paths.clear();
          app.operation_status = toast;
          app.operation_status_expires_ms = menu_expiry_3s();
        }
        app.op_progress.reset();
        conflict_cleanup(app);
        reload_dir(app);
        draw(app);
      },
      app.conflict_overwrite, dst_names);
}

void request_fs_operation(AppState& app, const std::vector<std::string>& srcs,
                          const std::string& dest_dir, bool is_move,
                          const std::string& success_toast, bool clear_cut) {
  conflict_cleanup(app);
  app.conflict_dest_dir = dest_dir;
  app.conflict_is_move = is_move;
  app.conflict_success_toast = success_toast;
  app.conflict_clear_cut = clear_cut;

  auto dest_base = dest_dir;
  while (dest_base.size() > 1 && dest_base.back() == '/')
    dest_base.pop_back();
  for (const auto& src : srcs) {
    if (!vfs_exists(src)) continue;

    std::string fname = fs::path(src).filename().string();
    std::string dp = dest_base + "/" + fname;

    // Dropping/pasting an item onto itself — duplicate under a unique name
    if (vfs_exists(dp) && vfs_equivalent(src, dp)) {
      fs::path spp(src);
      std::string stem = spp.stem().string();
      std::string ext = spp.extension().string();
      int n = 2;
      std::string unique = dp;
      while (vfs_exists(unique))
        unique = dest_base + "/" + stem + " (" + std::to_string(n++) + ")" + ext;
      app.conflict_srcs.push_back(src);
      app.conflict_dst_names.push_back(fs::path(unique).filename().string());
      continue;
    }

    app.conflict_srcs.push_back(src);

    if (vfs_exists(dp)) {
      AppState::ConflictEntry c;
      c.src = src;
      c.dest = dp;
      VfsInfo si = vfs_stat(src);
      VfsInfo di = vfs_stat(dp);
      c.src_is_dir = si.is_dir;
      c.dest_is_dir = di.is_dir;
      c.src_size = si.is_dir ? 0 : si.size;
      c.src_mtime = si.mtime;
      c.dest_size = di.is_dir ? 0 : di.size;
      c.dest_mtime = di.mtime;
      app.conflict_queue.push_back(std::move(c));
      app.conflict_dst_names.push_back("");
    } else {
      app.conflict_dst_names.push_back("");
    }
  }

  if (app.conflict_queue.empty()) {
    start_planned_fs_operation(app);
    draw(app);
    return;
  }
  app.conflict_open = true;
  app.conflict_apply_all = false;
  draw(app);
}

// dest_dir overrides the target directory (empty = current directory).

void paste_clipboard(AppState& app, const std::string& dest_dir) {
  const std::string& dest = dest_dir.empty() ? app.cur_tab().current_path : dest_dir;
  // Remote destinations are served by the GIO worker; other virtual views
  // (recent/starred/computer) have no backing directory to write to.
  if (app.is_virtual_path(dest) && !is_remote_uri(dest)) {
    app.operation_status = "Cannot paste into virtual views";
    app.operation_status_expires_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count() +
        3000;
    draw(app);
    return;
  }
  auto cf = app.clipboard.read_files(app.wl.display());
  if (!cf.paths.empty()) {
    request_fs_operation(app, cf.paths, dest, cf.is_cut,
                         cf.is_cut ? "Moved" : "Pasted", cf.is_cut);
    return;
  }

  // No files on the clipboard — try to save image data (e.g. a screenshot)
  std::string mime;
  std::string data = app.clipboard.read_image(&mime, app.wl.display());
  if (data.empty()) {
    draw(app);
    return;
  }

  std::string ext = ".png";
  if (mime.find("jpeg") != std::string::npos || mime.find("jpg") != std::string::npos) ext = ".jpg";
  else if (mime.find("webp") != std::string::npos) ext = ".webp";
  else if (mime.find("gif") != std::string::npos) ext = ".gif";
  else if (mime.find("bmp") != std::string::npos) ext = ".bmp";
  else if (mime.find("tiff") != std::string::npos) ext = ".tif";

  char ts[32]{};
  std::time_t t = std::time(nullptr);
  std::tm lt{};
  localtime_r(&t, &lt);
  std::strftime(ts, sizeof(ts), "%Y-%m-%d %H-%M-%S", &lt);

  const fs::path dir(dest);
  fs::path candidate = dir / ("Pasted Image " + std::string(ts) + ext);
  int n = 2;
  std::error_code ec;
  while (fs::exists(candidate, ec))
    candidate = dir / ("Pasted Image " + std::string(ts) + " (" + std::to_string(n++) + ")" + ext);

  {
    std::ofstream out(candidate, std::ios::binary | std::ios::trunc);
    if (!out) {
      draw(app);
      return;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
      fs::remove(candidate, ec);
      draw(app);
      return;
    }
  }

  AppState::UndoRecord rec{AppState::UndoRecord::Type::NewFile, {}, {}};
  rec.paths_b.push_back(candidate.string());
  app.redo_stack.clear();
  app.undo_stack.push_back(std::move(rec));
  if (app.undo_stack.size() > app.kMaxUndo)
    app.undo_stack.erase(app.undo_stack.begin());

  app.operation_status = "Pasted";
  app.operation_status_expires_ms = menu_expiry_3s();
  reload_dir(app);
  draw(app);
}

void resolve_conflict_choice(AppState& app, int choice) {
  if (!app.conflict_open || app.conflict_queue.empty()) return;

  // Button order must match draw_conflict_dialog / the click handler:
  // 0 = Skip, 1 = Cancel, 2 = Overwrite/Merge.
  if (choice == 1) {  // Cancel
    conflict_cleanup(app);
    draw(app);
    return;
  }

  const AppState::ConflictEntry cur = app.conflict_queue.front();
  bool apply_all = app.conflict_apply_all;

  if (choice == 2) {  // Overwrite / Merge
    if (apply_all) {
      for (const auto& c : app.conflict_queue)
        app.conflict_overwrite.push_back(c.src);
      app.conflict_queue.clear();
    } else {
      app.conflict_overwrite.push_back(cur.src);
      app.conflict_queue.erase(app.conflict_queue.begin());
    }
  } else {  // Skip
    if (apply_all) {
      for (const auto& c : app.conflict_queue)
        app.conflict_skipped.push_back(c.src);
      app.conflict_queue.clear();
    } else {
      app.conflict_skipped.push_back(cur.src);
      app.conflict_queue.erase(app.conflict_queue.begin());
    }
  }

  if (app.conflict_queue.empty()) {
    app.conflict_open = false;
    start_planned_fs_operation(app);
  }
  draw(app);
}


void open_context_menu(AppState& app, int item_idx, int x, int y) {
  // A right-click over a file must not leave its hover preview floating
  // above the menu; tear it down before the menu is built.
  if (app.preview_entry_idx >= 0) reset_preview(app);
  app.context_menu_open = true;
  app.context_menu_x = x;
  app.context_menu_y = y;
  app.context_menu_hover = -1; app.context_menu_hover_prev = -1; app.context_menu_sub_hover = -1;
  app.context_menu_sidebar_idx = -1;   // file-list menu, not a sidebar menu

  // Expanded tree rows don't exist in visible_entries, so index resolution
  // would bind to the wrong entry and actions like Delete silently no-op.
  // Materialize the row into context_menu_tree_entry and mark the menu with
  // the -9 sentinel; the action dispatcher binds to it instead.
  const bool tree_row =
      app.cur_tab().view_mode == ViewMode::Tree &&
      item_idx >= 0 &&
      item_idx < static_cast<int>(app.cur_tab().tree_entries.size());
  if (!tree_row)
    app.context_menu_tree_entry = FileEntry{};

  app.context_menu_file_idx = item_idx;

  std::string term_label = "Open in Terminal";
  {
    const auto& sc = eh::config::shell_config_snapshot();
    if (!sc.defaultApps.terminal.empty()) {
      scan_terminal_apps(app);
      for (const auto& ta : app.term_chooser_apps) {
        std::string stem = ta.desktop_id;
        auto slash = stem.rfind('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        auto dot = stem.rfind('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        if (stem == sc.defaultApps.terminal || ta.desktop_id == sc.defaultApps.terminal ||
            ta.desktop_id == sc.defaultApps.terminal + ".desktop") {
          term_label = "Open in " + ta.name;
          break;
        }
      }
    }
  }

  bool is_dir = false;
  bool is_executable = false;
  if (tree_row) {
    const auto& te = app.cur_tab().tree_entries[item_idx];
    FileEntry& ov = app.context_menu_tree_entry;
    ov = FileEntry{};
    ov.name = te.name;
    ov.path = te.path;
    ov.is_dir = te.is_dir;
    ov.type = te.type;
    if (!te.is_dir) {
      std::error_code sec;
      auto sz = fs::file_size(te.path, sec);
      if (!sec) ov.size = sz;
    }
    is_dir = te.is_dir;
    is_executable = te.type == FileType::Executable;
    app.cur_tab().selected_idx = item_idx;
    app.cur_tab().tree_selected_path = te.path;
    app.cur_tab().multi_selected.clear();
    app.context_menu_file_idx = -9;   // tree-path target sentinel
    app.context_menu_items = {
      AppState::menu_item(AppState::ContextMenuAction::Open, "Open"),
      AppState::menu_item(AppState::ContextMenuAction::OpenWith, "Open With..."),
    };
  } else if (item_idx >= 0 &&
      item_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
    app.cur_tab().selected_idx = item_idx;
    // Keep existing multi-selection if right-clicked item is already part of it
    if (std::find(app.cur_tab().multi_selected.begin(), app.cur_tab().multi_selected.end(), item_idx) == app.cur_tab().multi_selected.end()) {
      app.cur_tab().multi_selected = {item_idx};
    }
    app.context_menu_items = {
      AppState::menu_item(AppState::ContextMenuAction::Open, "Open"),
      AppState::menu_item(AppState::ContextMenuAction::OpenWith, "Open With..."),
    };
    int real_idx = app.cur_tab().visible_entries[item_idx];
    is_dir = real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()) &&
                  app.cur_tab().entries[real_idx].is_dir;
    is_executable = real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()) &&
                  app.cur_tab().entries[real_idx].type == FileType::Executable;
  }
  // Shared item construction for both entry-backed and tree-row targets.
  {
    if (is_executable) {
      app.context_menu_items.push_back(
        AppState::menu_item(AppState::ContextMenuAction::RunProgram, "Run as Program"));
    }
    if (is_dir) {
      app.context_menu_items.push_back(
        AppState::menu_item(AppState::ContextMenuAction::OpenInNewTab, "Open in new tab"));
      app.context_menu_items.push_back(
        AppState::menu_item(AppState::ContextMenuAction::OpenInNewWindow, "Open in new window"));
      app.context_menu_items.push_back(
        AppState::menu_item(AppState::ContextMenuAction::OpenInTerminal, term_label));
      if (::geteuid() != 0)
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::OpenAsAdmin, "Open as Administrator"));
      app.context_menu_items.push_back(
        AppState::menu_item(AppState::ContextMenuAction::AddToFavorites, "Add to Favorites"));
      // Default startup folder toggle (local directories only).
      {
        const std::string* sfpath = nullptr;
        if (tree_row) {
          sfpath = &app.context_menu_tree_entry.path;
        } else if (item_idx >= 0 &&
                   item_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
          int real_idx = app.cur_tab().visible_entries[item_idx];
          if (real_idx >= 0 &&
              real_idx < static_cast<int>(app.cur_tab().entries.size()))
            sfpath = &app.cur_tab().entries[real_idx].path;
        }
        if (sfpath && !sfpath->empty() && (*sfpath)[0] == '/') {
          if (app.startup_folder == *sfpath)
            app.context_menu_items.push_back(
              AppState::menu_item(AppState::ContextMenuAction::ClearStartupFolder,
                                  "Unset Default Folder"));
          else
            app.context_menu_items.push_back(
              AppState::menu_item(AppState::ContextMenuAction::SetStartupFolder,
                                  "Set as Default Folder"));
        }
      }
      // Local directories: analyze in Disk Usage (remote/virtual URIs
      // never start with '/').
      {
        const std::string* dupath = nullptr;
        if (tree_row) {
          dupath = &app.context_menu_tree_entry.path;
        } else if (item_idx >= 0 &&
                   item_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
          int real_idx = app.cur_tab().visible_entries[item_idx];
          if (real_idx >= 0 &&
              real_idx < static_cast<int>(app.cur_tab().entries.size()))
            dupath = &app.cur_tab().entries[real_idx].path;
        }
        if (dupath && !dupath->empty() && (*dupath)[0] == '/') {
          app.context_menu_items.push_back(
            AppState::menu_item(AppState::ContextMenuAction::DiskUsage, "Disk Usage"));
        }
      }
    }
    // Disk images get a top-level Mount/Unmount toggle so it is
    // visible without opening the Archive submenu.
    if (!is_dir) {
      const std::string* iso_path = nullptr;
      if (tree_row)
        iso_path = &app.context_menu_tree_entry.path;
      else if (item_idx >= 0 &&
               item_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
        int real_idx = app.cur_tab().visible_entries[item_idx];
        if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()))
          iso_path = &app.cur_tab().entries[real_idx].path;
      }
      if (iso_path && is_iso_image(*iso_path)) {
        bool mounted = !drives::UDisks2DriveService::instance().find_loop_for_file(*iso_path).empty();
        app.context_menu_items.push_back(AppState::menu_item(
            mounted ? AppState::ContextMenuAction::UnmountIso : AppState::ContextMenuAction::MountIso,
            mounted ? "Unmount" : "Mount"));
      }
    }
    app.context_menu_items.push_back(AppState::menu_separator());
    app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Copy, "Copy"));
    app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Paste, "Paste"));
    if (is_dir)
      app.context_menu_items.push_back(
        AppState::menu_item(AppState::ContextMenuAction::PasteInto, "Paste Into Folder"));
    app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Rename, "Rename"));
    if (tree_row) {
      // .hidden bookkeeping only applies to scanned directory entries.
    } else {
      int real_for_hide = (item_idx >= 0 &&
                           item_idx < static_cast<int>(app.cur_tab().visible_entries.size()))
                          ? app.cur_tab().visible_entries[static_cast<size_t>(item_idx)] : -1;
      bool hide_listed = real_for_hide >= 0 &&
                    real_for_hide < static_cast<int>(app.cur_tab().entries.size()) &&
                    app.cur_tab().entries[real_for_hide].in_hidden_file;
      if (hide_listed)
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::UnhideFile, "Unhide"));
      else
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::HideFile, "Hide"));
    }
    app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::OpenFileLocation, "Open File Location"));
    app.context_menu_items.push_back(AppState::menu_separator());

    // Copy To / Move To submenus (quick targets + browse)
    {
      const std::string home = home_dir();
      const std::string desktop = home + "/Desktop";
      bool has_other_pane =
          app.split_view; // other pane always exists while split

      AppState::ContextMenuItem copy_to;
      copy_to.action = AppState::ContextMenuAction::Separator;
      copy_to.label = "Copy To";
      copy_to.sub_items = {
        AppState::menu_item(AppState::ContextMenuAction::CopyToHome, "Home"),
        AppState::menu_item(AppState::ContextMenuAction::CopyToDesktop, "Desktop"),
      };
      if (has_other_pane)
        copy_to.sub_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::CopyToOtherPane, "Other Pane"));
      copy_to.sub_items.push_back(AppState::menu_separator());
      copy_to.sub_items.push_back(
        AppState::menu_item(AppState::ContextMenuAction::CopyTo, "Browse..."));
      app.context_menu_items.push_back(std::move(copy_to));

      AppState::ContextMenuItem move_to;
      move_to.action = AppState::ContextMenuAction::Separator;
      move_to.label = "Move To";
      move_to.sub_items = {
        AppState::menu_item(AppState::ContextMenuAction::MoveToHome, "Home"),
        AppState::menu_item(AppState::ContextMenuAction::MoveToDesktop, "Desktop"),
      };
      if (has_other_pane)
        move_to.sub_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::MoveToOtherPane, "Other Pane"));
      move_to.sub_items.push_back(AppState::menu_separator());
      move_to.sub_items.push_back(
        AppState::menu_item(AppState::ContextMenuAction::MoveTo, "Browse..."));
      app.context_menu_items.push_back(std::move(move_to));
    }

    // Actions submenu
    {
      AppState::ContextMenuItem actions_item;
      actions_item.action = AppState::ContextMenuAction::Separator;
      actions_item.label = "Actions";
      actions_item.sub_items = {
        AppState::menu_item(AppState::ContextMenuAction::CopyPath, "Copy Path"),
        AppState::menu_item(AppState::ContextMenuAction::Duplicate, "Duplicate"),
        AppState::menu_item(AppState::ContextMenuAction::CreateSymlink, "Create Symlink"),
        AppState::menu_separator(),
        AppState::menu_item(AppState::ContextMenuAction::Cut, "Cut"),
        AppState::menu_item(AppState::ContextMenuAction::PermanentDelete, "Permanent Delete"),
      };
      if (app.cur_tab().multi_selected.size() == 2 && compare_tool_available())
        actions_item.sub_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::CompareFiles, "Compare Files"));
      if (!is_dir)
        actions_item.sub_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::Checksums, "Checksums…"));
      {
        // Star toggle for files and folders (Starred virtual view parity).
        std::string star_target;
        if (tree_row) star_target = app.context_menu_tree_entry.path;
        else if (item_idx >= 0 &&
                 item_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
          int ri = app.cur_tab().visible_entries[item_idx];
          if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size()))
            star_target = app.cur_tab().entries[ri].path;
        }
        if (!star_target.empty()) {
          bool starred = app.is_starred(star_target);
          actions_item.sub_items.push_back(AppState::menu_item(
              starred ? AppState::ContextMenuAction::RemoveStar
                      : AppState::ContextMenuAction::AddStar,
              starred ? "Remove Star" : "Add Star"));
        }
      }
      if (is_dir && app.per_folder_props)
        actions_item.sub_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::ApplyPropsToSubfolders,
                              "Apply View Props to Subfolders..."));
      app.context_menu_items.push_back(std::move(actions_item));
    }

    // Archive submenu
    {
      AppState::ContextMenuItem archive_item;
      archive_item.action = AppState::ContextMenuAction::Separator;
      archive_item.label = "Archive";
      archive_item.sub_items = {
        AppState::menu_item(AppState::ContextMenuAction::Compress, "Compress..."),
      };
      if (!is_dir) {
        const std::string* archive_path = nullptr;
        if (tree_row)
          archive_path = &app.context_menu_tree_entry.path;
        else if (item_idx >= 0 &&
                 item_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
          int real_idx = app.cur_tab().visible_entries[static_cast<size_t>(item_idx)];
          if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()))
            archive_path = &app.cur_tab().entries[real_idx].path;
        }
        if (archive_path && is_archive_extension(*archive_path)) {
          archive_item.sub_items.push_back(AppState::menu_item(AppState::ContextMenuAction::BrowseArchive, "Browse Archive"));
          archive_item.sub_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Extract, "Extract"));
          archive_item.sub_items.push_back(AppState::menu_item(AppState::ContextMenuAction::ExtractTo, "Extract to..."));
        }
        // Disk images get a Mount/Unmount toggle (UDisks2 loop).
        if (archive_path && is_iso_image(*archive_path)) {
          bool mounted = !drives::UDisks2DriveService::instance().find_loop_for_file(*archive_path).empty();
          archive_item.sub_items.push_back(AppState::menu_item(
              mounted ? AppState::ContextMenuAction::UnmountIso : AppState::ContextMenuAction::MountIso,
              mounted ? "Unmount" : "Mount"));
        }
      }
      app.context_menu_items.push_back(std::move(archive_item));
    }

    // Share submenu (Dolphin parity, lite: copy paths + email attachments).
    {
      AppState::ContextMenuItem share_item;
      share_item.action = AppState::ContextMenuAction::Separator;
      share_item.label = "Share";
      share_item.sub_items = {
        AppState::menu_item(AppState::ContextMenuAction::CopyPath, "Copy Path"),
        AppState::menu_item(AppState::ContextMenuAction::ShareEmail,
                            "Send as Email Attachment…"),
      };
      app.context_menu_items.push_back(std::move(share_item));
    }

    // Tags submenu: toggle registry tags (local files only — xattrs
    // don't travel over SFTP/Drive). Data carries the tag name.
    {
      const std::string* tag_path = nullptr;
      if (!tree_row && item_idx >= 0 &&
          item_idx < static_cast<int>(app.cur_tab().visible_entries.size())) {
        int real_idx = app.cur_tab().visible_entries[static_cast<size_t>(item_idx)];
        if (real_idx >= 0 && real_idx < static_cast<int>(app.cur_tab().entries.size()))
          tag_path = &app.cur_tab().entries[real_idx].path;
      }
      if (tag_path && !tag_path->empty() && (*tag_path)[0] == '/') {
        AppState::ContextMenuItem tags_item;
        tags_item.action = AppState::ContextMenuAction::Separator;
        tags_item.label = "Tags";
        for (auto& t : tag_registry(app.tag_colors))
          tags_item.sub_items.push_back(AppState::menu_item(
              AppState::ContextMenuAction::TagToggle, t.name, t.name));
        app.context_menu_items.push_back(std::move(tags_item));
      }
    }

    // Services submenu: matching kio/servicemenus .desktop actions for the
    // selected targets (mime-filtered, Dolphin subset). Omitted when empty.
    {
      std::vector<ServiceTarget> targets;
      if (tree_row) {
        const auto& te = app.context_menu_tree_entry;
        ServiceTarget t;
        t.path = te.path;
        t.is_dir = te.is_dir;
        t.mime = te.is_dir ? "inode/directory" : "application/octet-stream";
        if (!t.path.empty()) targets.push_back(std::move(t));
      } else {
        auto add_entry = [&](int vis_idx) {
          if (vis_idx < 0 ||
              vis_idx >= static_cast<int>(app.cur_tab().visible_entries.size()))
            return;
          int ri = app.cur_tab().visible_entries[static_cast<size_t>(vis_idx)];
          if (ri < 0 || ri >= static_cast<int>(app.cur_tab().entries.size())) return;
          const auto& e = app.cur_tab().entries[static_cast<size_t>(ri)];
          ServiceTarget t;
          t.path = e.path;
          t.is_dir = e.is_dir;
          t.mime = e.mime_type.empty()
                       ? (e.is_dir ? "inode/directory" : "application/octet-stream")
                       : e.mime_type;
          targets.push_back(std::move(t));
        };
        for (int vi : app.cur_tab().multi_selected) add_entry(vi);
        if (targets.empty() && item_idx >= 0 &&
            item_idx < static_cast<int>(app.cur_tab().visible_entries.size()))
          add_entry(item_idx);
      }
      std::vector<AppState::ContextMenuItem> svc_items;
      collect_service_items(targets, svc_items);
      if (!svc_items.empty()) {
        AppState::ContextMenuItem svc_menu;
        svc_menu.action = AppState::ContextMenuAction::Separator;
        svc_menu.label = "Services";
        svc_menu.sub_items = std::move(svc_items);
        app.context_menu_items.push_back(std::move(svc_menu));
      }
    }

    app.context_menu_items.push_back(AppState::menu_separator());

    bool in_trash = false;
    {
      const char* home = std::getenv("HOME");
      if (home) {
        std::string trash_prefix = std::string(home) + "/.local/share/Trash/files";
        const auto& cp = app.cur_tab().current_path;
        if (cp == trash_prefix || (cp.size() > trash_prefix.size() && cp.compare(0, trash_prefix.size(), trash_prefix) == 0 && cp[trash_prefix.size()] == '/'))
          in_trash = true;
      }
    }

    if (in_trash) {
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::RestoreFromTrash, "Restore"));
      app.context_menu_items.push_back(AppState::menu_separator());
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::PermanentDelete, "Delete"));
    } else {
      app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::MoveToTrash, "Move to Trash"));
    }
    app.context_menu_items.push_back(AppState::menu_separator());
    app.context_menu_items.push_back(AppState::menu_item(AppState::ContextMenuAction::Properties, "Properties"));
  }
  if (!tree_row &&
      !(item_idx >= 0 &&
        item_idx < static_cast<int>(app.cur_tab().visible_entries.size()))) {
    if (app.in_virtual_view()) {
      // Virtual views are read-only aggregations: no create/paste/terminal.
      // Remote URIs get a working subset (create + paste via the GIO worker).
      if (is_remote_uri(app.cur_tab().current_path)) {
        app.context_menu_items = {
          AppState::menu_item(AppState::ContextMenuAction::NewFolder, "New Folder"),
          AppState::menu_item(AppState::ContextMenuAction::NewDocument, "New Document"),
          AppState::menu_separator(),
          AppState::menu_item(AppState::ContextMenuAction::Paste, "Paste"),
          AppState::menu_separator(),
          AppState::menu_item(AppState::ContextMenuAction::Reload, "Reload"),
          AppState::menu_separator(),
          AppState::menu_item(AppState::ContextMenuAction::SelectAll, "Select All"),
          AppState::menu_item(AppState::ContextMenuAction::InvertSelection, "Invert Selection"),
          AppState::menu_item(AppState::ContextMenuAction::SelectPattern, "Select by Pattern\u2026"),
        };
        return;
      }
      app.context_menu_items = {
        AppState::menu_item(AppState::ContextMenuAction::Reload, "Reload"),
        AppState::menu_separator(),
        AppState::menu_item(AppState::ContextMenuAction::SelectAll, "Select All"),
        AppState::menu_item(AppState::ContextMenuAction::InvertSelection, "Invert Selection"),
        AppState::menu_item(AppState::ContextMenuAction::SelectPattern, "Select by Pattern\u2026"),
      };
      if (app.cur_tab().current_path == "recent://") {
        app.context_menu_items.push_back(AppState::menu_separator());
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::ClearRecent, "Clear Recent"));
      }
      // Default startup folder toggle for the virtual views that support
      // it (My Computer / Recent / Starred).
      {
        const std::string& cur = app.cur_tab().current_path;
        if (startup_folder_ok(cur)) {
          app.context_menu_items.push_back(AppState::menu_separator());
          if (app.startup_folder == cur)
            app.context_menu_items.push_back(
              AppState::menu_item(AppState::ContextMenuAction::ClearStartupFolder,
                                  "Unset Default Folder"));
          else
            app.context_menu_items.push_back(
              AppState::menu_item(AppState::ContextMenuAction::SetStartupFolder,
                                  "Set as Default Folder"));
        }
      }
      return;
    }
    app.context_menu_items = {
      AppState::menu_item(AppState::ContextMenuAction::NewFolder, "New Folder"),
      AppState::menu_item(AppState::ContextMenuAction::NewDocument, "New Document"),
      AppState::menu_separator(),
      AppState::menu_item(AppState::ContextMenuAction::OpenInTerminal, term_label),
      (::geteuid() != 0
           ? AppState::menu_item(AppState::ContextMenuAction::OpenAsAdmin,
                                 "Open as Administrator")
           : AppState::menu_separator()),
      AppState::menu_separator(),
      AppState::menu_item(AppState::ContextMenuAction::Paste, "Paste"),
      AppState::menu_item(AppState::ContextMenuAction::SelectAll, "Select All"),
      AppState::menu_item(AppState::ContextMenuAction::InvertSelection, "Invert Selection"),
      AppState::menu_item(AppState::ContextMenuAction::SelectPattern, "Select by Pattern\u2026"),
      AppState::menu_separator(),
    };
    // Default startup folder toggle for the current folder. When the
    // stored default is stale (deleted), still offer to clear it.
    {
      const std::string& cur = app.cur_tab().current_path;
      bool cur_ok = startup_folder_ok(cur);
      bool stored_valid = startup_folder_ok(app.startup_folder);
      if (cur_ok && app.startup_folder == cur) {
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::ClearStartupFolder,
                              "Unset Default Folder"));
        app.context_menu_items.push_back(AppState::menu_separator());
      } else if (cur_ok) {
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::SetStartupFolder,
                              "Set as Default Folder"));
        app.context_menu_items.push_back(AppState::menu_separator());
      } else if (!app.startup_folder.empty() && !stored_valid) {
        app.context_menu_items.push_back(
          AppState::menu_item(AppState::ContextMenuAction::ClearStartupFolder,
                              "Unset Default Folder"));
        app.context_menu_items.push_back(AppState::menu_separator());
      }
    }
    app.context_menu_items.push_back(
      AppState::menu_item(AppState::ContextMenuAction::Properties, "Properties"));
    insert_template_submenu(app, 2);
    insert_scripts_submenu(app, app.context_menu_items.size() - 2);
  }

}

} // namespace eh::file_browser
