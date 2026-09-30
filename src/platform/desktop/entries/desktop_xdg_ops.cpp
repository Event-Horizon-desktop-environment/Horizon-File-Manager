#include "platform/desktop/entries/desktop_xdg_ops.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace fs = std::filesystem;

namespace eh::shell::desktop::xdg {

namespace {

// Single-quote a shell argument so paths with spaces, quotes, $ or ` cannot
// break out into command injection.
std::string sh_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += '\'';
  return out;
}

std::string percent_decode(const std::string& s) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

}  // namespace

std::string canonical_absolute_path(const std::string& path) { return path; }

std::string file_uri_for_path(const std::string& abs_path) {
  return std::string("file://") + abs_path;
}

std::string expand_desktop_exec_tokens(std::string exec, const std::string&, const std::string&) {
  return exec;
}

void spawn_sh_lc_detached(const std::string& script) {
  std::system(("sh -c " + sh_quote(script) + " &").c_str());
}

void open_path_in_default_application(const std::string& abs_path) {
  std::string cmd = "xdg-open " + sh_quote(abs_path) + " &";
  std::system(cmd.c_str());
}

void open_uri(const std::string& uri) {
  std::string cmd = "xdg-open " + sh_quote(uri) + " &";
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
    std::string cmd = "horizon-files " + sh_quote(dir) + " &";
    std::system(cmd.c_str());
  }
}

void clipboard_files_cut_copy(bool, const std::string&) {}

void clipboard_files_cut_copy_multi(bool, const std::vector<std::string>&) {}

bool clipboard_paste_into_directory(const std::string&, std::vector<std::string>*, bool*) {
  return false;
}

bool trash_file(const std::string& abs_path) {
  std::string cmd = "gio trash " + sh_quote(abs_path) + " 2>/dev/null";
  return std::system(cmd.c_str()) == 0;
}

namespace {

// Recursive byte size (no symlink following); errors count as zero.
uint64_t trash_tree_bytes(const std::string& path) {
  std::error_code ec;
  uint64_t total = 0;
  struct stat dst {};
  bool is_dir = ::stat(path.c_str(), &dst) == 0 && S_ISDIR(dst.st_mode);
  if (is_dir) {
    fs::recursive_directory_iterator end;
    for (fs::recursive_directory_iterator
             it(path, fs::directory_options::skip_permission_denied, ec);
         !ec && it != end; it.increment(ec)) {
      if (ec) break;
      std::error_code ec2;
      if (it->is_regular_file(ec2) && !ec2) {
        struct stat st {};
        if (::stat(it->path().c_str(), &st) == 0)
          total += static_cast<uint64_t>(st.st_size);
      }
    }
    return total;
  }
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
    return static_cast<uint64_t>(st.st_size);
  return 0;
}

// Parse "[Trash Info]" DeletionDate "YYYY-MM-DDTHH:MM:SS" (local naive).
// Returns -1 when absent/unparseable: those entries are never auto-purged.
int64_t trash_deletion_epoch(const std::string& info_path) {
  std::ifstream f(info_path);
  if (!f.is_open()) return -1;
  std::string line;
  while (std::getline(f, line)) {
    if (line.compare(0, 13, "DeletionDate=") != 0) continue;
    int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
    if (std::sscanf(line.c_str() + 13, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h,
                    &m, &s) != 6)
      return -1;
    struct tm tm {};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    std::time_t t = std::mktime(&tm);
    return t == static_cast<std::time_t>(-1) ? -1
                                             : static_cast<int64_t>(t);
  }
  return -1;
}

} // namespace

void trash_maintain(int max_age_days, uint64_t max_bytes) {
  if (max_age_days <= 0 && max_bytes == 0) return;
  const char* home = std::getenv("HOME");
  if (!home || !*home) return;
  const std::string files_dir = std::string(home) + "/.local/share/Trash/files";
  const std::string info_dir = std::string(home) + "/.local/share/Trash/info";
  struct Entry {
    std::string info;
    std::string file;
    int64_t deleted = -1;
    uint64_t bytes = 0;
  };
  std::vector<Entry> entries;
  std::error_code ec;
  fs::directory_iterator it(info_dir, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (ec) break;
    std::error_code ec2;
    if (!it->is_regular_file(ec2) || ec2) continue;
    std::string iname = it->path().filename().string();
    if (iname.size() < 11 ||
        iname.compare(iname.size() - 10, 10, ".trashinfo") != 0)
      continue;
    std::string base = iname.substr(0, iname.size() - 10);
    Entry e;
    e.info = it->path().string();
    e.file = files_dir + "/" + base;
    e.deleted = trash_deletion_epoch(e.info);
    entries.push_back(std::move(e));
  }
  std::time_t now = std::time(nullptr);
  // Age purge first (dated entries only).
  if (max_age_days > 0) {
    const int64_t cutoff =
        static_cast<int64_t>(now) - int64_t{86400} * max_age_days;
    for (auto& e : entries) {
      if (e.deleted < 0 || e.deleted > cutoff) continue;
      std::error_code ec3;
      fs::remove_all(e.file, ec3);
      fs::remove(e.info, ec3);
      e.file.clear(); // gone: exclude from quota math below
    }
  }
  // Quota purge (dated entries, oldest deletion first).
  if (max_bytes > 0) {
    for (auto& e : entries) {
      if (!e.file.empty()) e.bytes = trash_tree_bytes(e.file);
    }
    uint64_t total = 0;
    for (auto& e : entries) total += e.bytes;
    if (total > max_bytes) {
      std::vector<Entry*> dated;
      for (auto& e : entries) {
        if (!e.file.empty() && e.deleted >= 0) dated.push_back(&e);
      }
      std::sort(dated.begin(), dated.end(), [](const Entry* a,
                                               const Entry* b) {
        return a->deleted < b->deleted;
      });
      for (auto* e : dated) {
        if (total <= max_bytes) break;
        std::error_code ec3;
        fs::remove_all(e->file, ec3);
        fs::remove(e->info, ec3);
        total -= std::min(total, e->bytes);
        e->file.clear();
      }
    }
  }
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

  // .trashinfo stores Path= percent-encoded (spaces as %20); decode it or
  // restore fails, and never silently overwrite an existing destination.
  original_path = percent_decode(original_path);
  if (original_path.empty()) return false;
  {
    std::ifstream probe(original_path);
    if (probe.is_open()) return false;
  }

  std::string mkdir_cmd = "mkdir -p " + sh_quote(original_path.substr(0, original_path.rfind('/'))) + " 2>/dev/null";
  std::system(mkdir_cmd.c_str());

  std::string mv_cmd = "mv " + sh_quote(trash_file_path) + " " + sh_quote(original_path) + " 2>/dev/null";
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
