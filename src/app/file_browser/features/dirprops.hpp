#pragma once

#include <functional>
#include <string>

namespace eh::file_browser {

// Per-folder view properties persisted as a `.directory` desktop-entry-style
// INI file (freedesktop convention, compatible-ish with Dolphin's file name).
struct DirProps {
  // Which keys the file actually carries
  bool has_mode = false;
  bool has_sort = false;
  bool has_flags = false;
  bool has_group = false;
  bool has_zoom = false;
  bool has_hidden = false;

  int mode = 0;                 // ViewMode: 0=List 1=Grid 2=Computer 3=Tree 4=Compact
  int sort_field = 0;           // SortField index
  bool sort_descending = false;
  bool natural = true;
  bool case_sensitive = false;
  bool hidden_last = false;
  bool folders_first = true;
  int group_field = 0;          // 0=None 1=Type 2=Name 3=Date 4=Size
  int zoom_level = 8;           // discrete zoom level (Phase 4.1)
  bool show_hidden = false;
};

std::string dir_props_path(const std::string& dir);
bool read_dir_props(const std::string& dir, DirProps* out);
bool write_dir_props(const std::string& dir, const DirProps& props);

// Recursively write props into every subfolder of `root` (not root itself).
// `cancelled` polled between entries. Returns number of folders written.
int apply_dir_props_recursive(const std::string& root, const DirProps& props,
                              const std::function<bool()>& cancelled);

} // namespace eh::file_browser
