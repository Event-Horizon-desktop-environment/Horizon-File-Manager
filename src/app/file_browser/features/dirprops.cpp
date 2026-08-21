#include "dirprops.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <sys/stat.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace eh::file_browser {

namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool parse_bool(const std::string& v, bool dflt) {
  std::string t = trim(v);
  for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (t == "true" || t == "1" || t == "yes") return true;
  if (t == "false" || t == "0" || t == "no") return false;
  return dflt;
}

int parse_int(const std::string& v, int dflt) {
  try {
    return std::stoi(trim(v));
  } catch (...) {
    return dflt;
  }
}

} // namespace

std::string dir_props_path(const std::string& dir) {
  return dir + "/.directory";
}

bool read_dir_props(const std::string& dir, DirProps* out) {
  if (!out) return false;
  std::ifstream f(dir_props_path(dir));
  if (!f) return false;

  DirProps p{};
  std::string line;
  std::string group;
  bool any = false;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      group = line.substr(1, line.size() - 2);
      for (auto& c : group)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      continue;
    }
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    for (auto& c : key)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (group != "horizon view") {
      if (group != "desktop entry") continue;
      // Desktop-entry group: only tolerate standard keys we write ourselves
      continue;
    }

    any = true;
    if (key == "mode") { p.mode = parse_int(val, 0); p.has_mode = true; }
    else if (key == "sort") { p.sort_field = parse_int(val, 0); p.has_sort = true; }
    else if (key == "sortdescending") { p.sort_descending = parse_bool(val, false); p.has_sort = true; }
    else if (key == "natural") { p.natural = parse_bool(val, true); p.has_flags = true; }
    else if (key == "casesensitive") { p.case_sensitive = parse_bool(val, false); p.has_flags = true; }
    else if (key == "hiddenlast") { p.hidden_last = parse_bool(val, false); p.has_flags = true; }
    else if (key == "foldersfirst") { p.folders_first = parse_bool(val, true); p.has_flags = true; }
    else if (key == "groupfield") { p.group_field = parse_int(val, 0); p.has_group = true; }
    else if (key == "zoomlevel") { p.zoom_level = parse_int(val, 8); p.has_zoom = true; }
    else if (key == "showhidden") { p.show_hidden = parse_bool(val, false); p.has_hidden = true; }
  }

  if (!any) return false;
  *out = p;
  return true;
}

bool write_dir_props(const std::string& dir, const DirProps& props) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
  std::string path = dir_props_path(dir);

  // Refuse to fight read-only locations
  if (fs::exists(path, ec)) {
    FILE* probe = fopen(path.c_str(), "r+");
    if (!probe) return false;
    fclose(probe);
  } else {
    FILE* probe = fopen(path.c_str(), "wx");
    if (!probe) return false;
    fclose(probe);
    fs::remove(path, ec);
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;

  out << "[Desktop Entry]\n";
  out << "Type=Directory\n";
  out << "\n[Horizon View]\n";
  if (props.has_mode) out << "Mode=" << props.mode << "\n";
  if (props.has_sort) {
    out << "Sort=" << props.sort_field << "\n";
    out << "SortDescending=" << (props.sort_descending ? "true" : "false") << "\n";
  }
  if (props.has_flags) {
    out << "Natural=" << (props.natural ? "true" : "false") << "\n";
    out << "CaseSensitive=" << (props.case_sensitive ? "true" : "false") << "\n";
    out << "HiddenLast=" << (props.hidden_last ? "true" : "false") << "\n";
    out << "FoldersFirst=" << (props.folders_first ? "true" : "false") << "\n";
  }
  if (props.has_group) out << "GroupField=" << props.group_field << "\n";
  if (props.has_zoom) out << "ZoomLevel=" << props.zoom_level << "\n";
  if (props.has_hidden) out << "ShowHidden=" << (props.show_hidden ? "true" : "false") << "\n";
  return out.good();
}

int apply_dir_props_recursive(const std::string& root, const DirProps& props,
                              const std::function<bool()>& cancelled) {
  int written = 0;
  std::error_code ec;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  if (ec) return 0;
  for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (cancelled && cancelled()) break;
    if (ec) { ec.clear(); continue; }
    const fs::directory_entry& de = *it;
    std::error_code ec2;
    if (!de.is_directory(ec2)) continue;
    if (write_dir_props(de.path().string(), props)) ++written;
  }
  return written;
}

} // namespace eh::file_browser
