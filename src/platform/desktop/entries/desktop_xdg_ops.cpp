#include "platform/desktop/entries/desktop_xdg_ops.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace eh::shell::desktop::xdg {

std::string canonical_absolute_path(const std::string& path) { return path; }

std::string file_uri_for_path(const std::string& abs_path) {
  return std::string("file://") + abs_path;
}

std::string expand_desktop_exec_tokens(std::string exec, const std::string&, const std::string&) {
  return exec;
}

void spawn_sh_lc_detached(const std::string& script) {
  std::system(("sh -c '" + script + "' &").c_str());
}

void open_path_in_default_application(const std::string& abs_path) {
  std::string cmd = "xdg-open " + abs_path + " &";
  std::system(cmd.c_str());
}

void open_uri(const std::string& uri) {
  std::string cmd = "xdg-open '" + uri + "' &";
  std::system(cmd.c_str());
}

bool dbus_filemanager_show_items_select_uri(const std::string&) { return false; }

void open_properties_for_desktop_file(const std::string&) {}

void open_file_location(const std::string& desktop_abs_path, const DesktopEntryInfo*) {
  // Determine the directory containing the .desktop file and open it
  // in the file manager. We rely on the file manager being on PATH.
  auto slash = desktop_abs_path.rfind('/');
  if (slash != std::string::npos) {
    std::string dir = desktop_abs_path.substr(0, slash);
    std::string cmd = "horizon-files '" + dir + "' &";
    std::system(cmd.c_str());
  }
}

void clipboard_files_cut_copy(bool, const std::string&) {}

void clipboard_files_cut_copy_multi(bool, const std::vector<std::string>&) {}

bool clipboard_paste_into_directory(const std::string&, std::vector<std::string>*, bool*) {
  return false;
}

bool trash_file(const std::string& abs_path) {
  std::string cmd = "gio trash '" + abs_path + "' 2>/dev/null";
  return std::system(cmd.c_str()) == 0;
}

bool restore_from_trash(const std::string& trash_file_path) {
  const char* home = std::getenv("HOME");
  if (!home) return false;

  std::string trash_prefix = std::string(home) + "/.local/share/Trash/files/";
  if (trash_file_path.find(trash_prefix) != 0) return false;

  std::string filename = trash_file_path.substr(trash_prefix.size());

  std::string info_path = std::string(home) + "/.local/share/Trash/info/" + filename + ".trashinfo";

  std::ifstream info(info_path);
  if (!info.is_open()) return false;

  std::string original_path;
  std::string line;
  while (std::getline(info, line)) {
    if (line.compare(0, 5, "Path=") == 0) {
      original_path = line.substr(5);
      break;
    }
  }
  info.close();

  if (original_path.empty()) return false;

  std::string mkdir_cmd = "mkdir -p '" + original_path.substr(0, original_path.rfind('/')) + "' 2>/dev/null";
  std::system(mkdir_cmd.c_str());

  std::string mv_cmd = "mv '" + trash_file_path + "' '" + original_path + "' 2>/dev/null";
  int ret = std::system(mv_cmd.c_str());
  if (ret != 0) return false;

  std::remove(info_path.c_str());
  return true;
}

void launch_expanded_exec_line(const std::string& expanded_exec, bool) {
  std::system((expanded_exec + " &").c_str());
}

void launch_pkexec_exec_raw(const std::string&, const std::string&, const std::string&, bool) {}

void open_desktop_default(const std::string&, const std::optional<DesktopEntryInfo>&,
                           const std::string&) {}

void launch_action_exec(const std::string&, const std::string&, const std::string&, bool) {}

std::string prompt_rename_text(const std::string&, const std::string&) { return {}; }

}
