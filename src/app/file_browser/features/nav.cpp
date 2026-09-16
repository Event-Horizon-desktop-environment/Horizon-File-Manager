// nav.cpp
// Moved wholesale from features/nav.cpp (byte-identical bodies).

// Must come first: exposes struct statx through <sys/stat.h>.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "../trace.hpp"

#include "../app.hpp"

#include <chrono>

#include "app/file_browser/features/desktop_icon_parser.hpp"
#include "app/file_browser/features/dirprops.hpp"
#include "app/file_browser/features/query_match.hpp"
#include "app/file_browser/features/recursive_search_worker.hpp"
#include "app/file_browser/features/tab_history.hpp"
#include "app/file_browser/features/thumb_pool.hpp"
#include "app/file_browser/features/nav.hpp"



#include "app/file_browser/features/video_worker.hpp"
#include "app/file_browser/features/svg_preview.hpp"
#include "app/file_browser/features/pdf_preview.hpp"
#include "app/file_browser/features/epub_preview.hpp"
#include "app/file_browser/features/image_preview.hpp"
#include "app/file_browser/features/view_zoom.hpp"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mntent.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#if defined(EH_HAVE_IO_URING)
#include <liburing.h>
#endif

#include "services/udisks2/drive_filter.hpp"

#include <gio/gio.h>

#include "services/udisks2/udisks2_drive_service.hpp"

#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "platform/common/ns/namespaces.hpp"
#include "wayland/surface/layer_surface.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;

namespace eh::file_browser {

void preview_log(const char* fmt, ...);  // defined in draw.cpp

// ── helpers ──────────────────────────────────────────────────────


bool is_hidden_file(const std::string& name) {
  return !name.empty() && name[0] == '.';
}

// Names listed in a directory's `.hidden` file are treated as hidden too
// (freedesktop convention, honored by Dolphin/Nemo).
std::unordered_set<std::string> read_hidden_file(const std::string& dir) {
  std::unordered_set<std::string> names;
  // Never block while scanning: a FIFO named ".hidden" would hang a plain
  // ifstream open forever. Open non-blocking and only read regular files.
  int fd = ::open((dir + "/.hidden").c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return names;
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return names;
  }
  std::string data;
  char buf[8192];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    data.append(buf, static_cast<size_t>(n));
    if (data.size() > (1u << 20)) break;  // safety cap; .hidden files are tiny
  }
  ::close(fd);
  std::istringstream in(data);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) names.insert(line);
  }
  return names;
}

// ── app state constructor ────────────────────────────────────────

AppState::AppState() {
  tabs.emplace_back();
  tabs[0].current_path = home_dir();
}

AppState::~AppState() {
  if (dir_watch_fd >= 0) {
    ::close(dir_watch_fd);  // defensive: teardown already calls dir_watch_close
    dir_watch_fd = -1;
  }
  if (arrow_left_svg) cairo_surface_destroy(arrow_left_svg);
  if (arrow_right_svg) cairo_surface_destroy(arrow_right_svg);
  if (arrow_up_svg) cairo_surface_destroy(arrow_up_svg);
  if (search_svg) cairo_surface_destroy(search_svg);
  if (folder_search_svg) cairo_surface_destroy(folder_search_svg);
  if (mounted_svg) cairo_surface_destroy(mounted_svg);
  if (sidebar_toggle_svg) cairo_surface_destroy(sidebar_toggle_svg);
  if (sort_chevron_svg) cairo_surface_destroy(sort_chevron_svg);
  if (checkmark_svg) cairo_surface_destroy(checkmark_svg);
  if (arrow_down_svg) cairo_surface_destroy(arrow_down_svg);
  if (arrow_downward_svg) cairo_surface_destroy(arrow_downward_svg);
  if (icon_hash_svg) cairo_surface_destroy(icon_hash_svg);
  if (icon_bars_svg) cairo_surface_destroy(icon_bars_svg);
  if (icon_clock_svg) cairo_surface_destroy(icon_clock_svg);
  if (icon_file_text_svg) cairo_surface_destroy(icon_file_text_svg);
  if (icon_person_svg) cairo_surface_destroy(icon_person_svg);
  if (icon_people_svg) cairo_surface_destroy(icon_people_svg);
  if (icon_shield_svg) cairo_surface_destroy(icon_shield_svg);
  if (icon_file_svg) cairo_surface_destroy(icon_file_svg);
  if (icon_link_svg) cairo_surface_destroy(icon_link_svg);
  if (icon_folder_svg) cairo_surface_destroy(icon_folder_svg);
  if (icon_eyeoff_svg) cairo_surface_destroy(icon_eyeoff_svg);
  if (icon_list_svg) cairo_surface_destroy(icon_list_svg);
  if (icon_aa_svg) cairo_surface_destroy(icon_aa_svg);
  if (icon_minus_svg) cairo_surface_destroy(icon_minus_svg);
  if (edit_svg) cairo_surface_destroy(edit_svg);
  if (lock_svg) cairo_surface_destroy(lock_svg);
  if (trash_svg) cairo_surface_destroy(trash_svg);
  if (monitor_svg) cairo_surface_destroy(monitor_svg);
  if (icon_desktop_svg) cairo_surface_destroy(icon_desktop_svg);
  if (icon_documents_svg) cairo_surface_destroy(icon_documents_svg);
  if (icon_downloads_svg) cairo_surface_destroy(icon_downloads_svg);
  if (icon_music_svg) cairo_surface_destroy(icon_music_svg);
  if (icon_pictures_svg) cairo_surface_destroy(icon_pictures_svg);
  if (icon_videos_svg) cairo_surface_destroy(icon_videos_svg);
  if (icon_publicshare_svg) cairo_surface_destroy(icon_publicshare_svg);
  if (icon_templates_svg) cairo_surface_destroy(icon_templates_svg);
}

// ── home_dir ─────────────────────────────────────────────────────

std::string home_dir() {
  if (auto* h = std::getenv("HOME")) return h;
  if (auto* pw = getpwuid(getuid())) return pw->pw_dir;
  return "/";
}

// ── directory listing ────────────────────────────────────────────

void reset_scroll_and_selection(AppState& app) {
  app.cur_tab().hover_idx = -1;
  app.cur_tab().selected_idx = -1;
  app.cur_tab().multi_selected.clear();
  app.cur_tab().scroll_px = 0;
  app.cur_tab().scroll_smooth_current = 0.0;
  app.cur_tab().scroll_smooth_target = 0.0;
}

bool matches_filter(const AppState& app, const FileEntry& entry) {
  int ft = app.active_pane ? app.r_filter_type_idx : app.filter_type_idx;
  int fs = app.active_pane ? app.r_filter_size_idx : app.filter_size_idx;
  int fd = app.active_pane ? app.r_filter_date_idx : app.filter_date_idx;
  // Type filter
  if (ft > 0) {
    FileType target = static_cast<FileType>(ft - 1);
    if (entry.type != target) return false;
  }
  // Size filter
  if (fs > 0) {
    uint64_t s = entry.size;
    switch (fs) {
      case 1: if (s >= 10240) return false; break;
      case 2: if (s < 10240 || s >= 102400) return false; break;
      case 3: if (s < 102400 || s >= 1048576) return false; break;
      case 4: if (s < 1048576 || s >= 10485760) return false; break;
      case 5: if (s < 10485760 || s >= 104857600) return false; break;
      case 6: if (s < 104857600) return false; break;
    }
  }
  // Date filter
  if (fd > 0) {
    int64_t mod = entry.modified_sec;
    if (mod == 0) return false;
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int64_t boundary = 0;
    switch (fd) {
      case 1: { // Today
        struct tm tm_today = tm_now;
        tm_today.tm_hour = 0; tm_today.tm_min = 0; tm_today.tm_sec = 0;
        boundary = mktime(&tm_today);
        break;
      }
      case 2: { // This week (start of Monday)
        int days_since_monday = (tm_now.tm_wday + 6) % 7;
        struct tm tm_week = tm_now;
        tm_week.tm_hour = 0; tm_week.tm_min = 0; tm_week.tm_sec = 0;
        tm_week.tm_mday -= days_since_monday;
        boundary = mktime(&tm_week);
        break;
      }
      case 3: { // This month
        struct tm tm_month = tm_now;
        tm_month.tm_hour = 0; tm_month.tm_min = 0; tm_month.tm_sec = 0;
        tm_month.tm_mday = 1;
        boundary = mktime(&tm_month);
        break;
      }
      case 4: { // This year
        struct tm tm_year = tm_now;
        tm_year.tm_hour = 0; tm_year.tm_min = 0; tm_year.tm_sec = 0;
        tm_year.tm_mday = 1; tm_year.tm_mon = 0;
        boundary = mktime(&tm_year);
        break;
      }
    }
    if (mod < boundary) return false;
  }
  return true;
}

// Query match honoring the filter-bar mode (plain/glob/regex) and case
// toggle. Empty query matches everything.
bool query_matches_entry(const AppState& app, const std::string& name) {
  const auto& q = app.active_pane ? app.r_search_query : app.search_query;
  if (q.empty()) return true;
  int mode = app.active_pane ? app.r_search_mode : app.search_mode;
  bool cs = app.active_pane ? app.r_search_case_sensitive : app.search_case_sensitive;
  bool valid = true;
  return name_matches(name, q, mode, cs, &valid);
}

// Restart the active pane's search with current query/mode/case/filter
// settings (used after mode or option changes).
void restart_active_search(AppState& app) {
  const auto& q = app.active_pane ? app.r_search_query : app.search_query;
  bool rec = app.active_pane ? app.r_recursive_search_active
                             : app.recursive_search_active;
  if (q.empty() || !(app.search_active || app.recursive_search_active ||
                     app.r_search_active || app.r_recursive_search_active)) {
    recursive_search_worker().cancel();
    return;
  }
  app.cur_tab().entries.clear();
  app.cur_tab().visible_entries.clear();
  ++app.listing_epoch;  // scroll-delta content reuse invalid

  SearchOptions opt;
  opt.mode = app.active_pane ? app.r_search_mode : app.search_mode;
  opt.case_sensitive = app.active_pane ? app.r_search_case_sensitive
                                       : app.search_case_sensitive;
  int ft = app.active_pane ? app.r_filter_type_idx : app.filter_type_idx;
  int fs = app.active_pane ? app.r_filter_size_idx : app.filter_size_idx;
  int fd = app.active_pane ? app.r_filter_date_idx : app.filter_date_idx;
  if (ft > 0 || fs > 0 || fd > 0) {
    opt.predicate = [ft, fs, fd](const std::string& path,
                                 const std::string& name, bool is_dir,
                                 uint64_t size, int64_t mtime) {
      return search_predicate_passes(ft, fs, fd, path, name, is_dir, size,
                                     mtime);
    };
  }
  std::string root = rec ? home_dir() : app.cur_tab().current_path;
  recursive_search_worker().start_search(root, q, opt);
}

void reset_search_filters(AppState& app) {
  (app.active_pane ? app.r_filter_type_idx : app.filter_type_idx) = 0;
  (app.active_pane ? app.r_filter_size_idx : app.filter_size_idx) = 0;
  (app.active_pane ? app.r_filter_date_idx : app.filter_date_idx) = 0;
  (app.active_pane ? app.r_filter_dropdown_section : app.filter_dropdown_section) = 0;
  (app.active_pane ? app.r_filter_dropdown_hover : app.filter_dropdown_hover) = -1;
}

void trigger_search_on_filter_change(AppState& app) {
  if ((app.active_pane ? app.r_filter_type_idx : app.filter_type_idx) == 0 && (app.active_pane ? app.r_filter_size_idx : app.filter_size_idx) == 0 && (app.active_pane ? app.r_filter_date_idx : app.filter_date_idx) == 0) {
    // No filters — just re-apply the existing apply_filter logic
    apply_filter(app);
    return;
  }
  // Rebuild visible_entries from entries with filters
  ++app.listing_epoch;  // scroll-delta content reuse invalid
  app.cur_tab().visible_entries.clear();
  for (int i = 0; i < static_cast<int>(app.cur_tab().entries.size()); ++i) {
    if (!app.show_hidden && app.cur_tab().entries[i].is_hidden) continue;
    if (!query_matches_entry(app, app.cur_tab().entries[i].name)) continue;
    if (!matches_filter(app, app.cur_tab().entries[i])) continue;
    app.cur_tab().visible_entries.push_back(i);
  }
  reset_scroll_and_selection(app);
  app.cur_tab().tree_entries_dirty = true;
}

void apply_filter(AppState& app) {
  if (app.cur_tab().current_path == "computer://") return;
  ++app.listing_epoch;  // scroll-delta content reuse invalid
  app.cur_tab().visible_entries.clear();
  for (int i = 0; i < static_cast<int>(app.cur_tab().entries.size()); ++i) {
    if (!app.show_hidden && app.cur_tab().entries[i].is_hidden) continue;
    if (!query_matches_entry(app, app.cur_tab().entries[i].name)) continue;
    if (!matches_filter(app, app.cur_tab().entries[i])) continue;
    app.cur_tab().visible_entries.push_back(i);
  }
  reset_scroll_and_selection(app);
  app.cur_tab().tree_entries_dirty = true;
}


std::string format_mode(uint32_t mode) {
  std::string s = "---------";
  if (mode & 0400) s[0] = 'r';
  if (mode & 0200) s[1] = 'w';
  if (mode & 0100) s[2] = 'x';
  if (mode & 0040) s[3] = 'r';
  if (mode & 0020) s[4] = 'w';
  if (mode & 0010) s[5] = 'x';
  if (mode & 0004) s[6] = 'r';
  if (mode & 0002) s[7] = 'w';
  if (mode & 0001) s[8] = 'x';
  return s;
}

std::string group_label_for(const AppState& app, const FileEntry& e) {
  switch (app.cur_tab().group_field) {
    case 1:
      switch (e.type) {
        case FileType::Folder:     return "Folders";
        case FileType::Image:      return "Images";
        case FileType::Audio:      return "Audio";
        case FileType::Video:      return "Videos";
        case FileType::Text:       return "Text";
        case FileType::Markdown:   return "Markdown";
        case FileType::Code:       return "Code Files";
        case FileType::Document:   return "Documents";
        case FileType::Font:       return "Fonts";
        case FileType::Archive:    return "Archives";
        case FileType::Executable: return "Executables";
        case FileType::Web:        return "Web";
        default:                   return "Other Files";
      }
    case 2: {
      if (e.name.empty()) return "#";
      unsigned char c0 = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(e.name[0])));
      if (c0 >= 'a' && c0 <= 'z') {
        std::string s(1, static_cast<char>(std::toupper(c0)));
        return s;
      }
      return "#";
    }
    case 3: {
      std::time_t now = std::time(nullptr);
      double age_s = difftime(now, e.modified_sec);
      if (age_s < 0) age_s = 0;
      if (age_s < 86400) return "Today";
      if (age_s < 172800) return "Yesterday";
      if (age_s < 7 * 86400) return "This Week";
      if (age_s < 30 * 86400) return "This Month";
      if (age_s < 365 * 86400) return "This Year";
      return "Earlier";
    }
    case 4:
      if (e.is_dir) return "Folders";
      if (e.size < 100ull * 1024) return "Small (<100 KB)";
      if (e.size < 10ull * 1024 * 1024) return "Medium (<10 MB)";
      if (e.size < 1024ull * 1024 * 1024) return "Large (<1 GB)";
      return "Huge (≥1 GB)";
    default: return "";
  }
}

void clear_thumb_cache(AppState& app) {
  for (auto& [_, s] : app.thumb_cache) {
    if (s) cairo_surface_destroy(s);
  }
  app.thumb_cache.clear();
  app.thumb_lru.clear();
  app.thumb_cache_bytes = 0;
  app.thumb_pending_queue.clear();
}

// ── navigation ───────────────────────────────────────────────────

// Snapshot current view state into DirProps for per-folder persistence
static void save_dir_props_before_leave(AppState& app) {
  if (!app.per_folder_props) return;
  const std::string old_dir = app.cur_tab().current_path;
  if (old_dir.empty() || old_dir == "computer://" || old_dir == "trash://" ||
      old_dir.rfind("recent://", 0) == 0)
    return;

  DirProps p;
  p.has_mode = true;
  p.mode = static_cast<int>(app.cur_tab().view_mode);
  if (p.mode == static_cast<int>(ViewMode::Computer)) return;
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

  write_dir_props(old_dir, p);
}

// Snapshot the current tab's view settings into the per-directory cache
static void remember_independent_view(AppState& app) {
  if (!app.independent_dir_views) return;
  const std::string& p = app.cur_tab().current_path;
  if (p.empty() || p == "computer://" || p == "trash://" ||
      p.rfind("recent://", 0) == 0)
    return;
  auto& t = app.cur_tab();
  AppState::DirViewState st;
  st.view_mode = t.view_mode;
  st.sort_field = t.sort_field;
  st.sort_descending = t.sort_descending;
  st.group_by_type = t.group_by_type;
  st.group_field = t.group_field;
  st.zoom_level = zoom_level_for_pct(app.zoom_pct);
  app.dir_view_states[p] = st;
}

// Restore the cached view settings for `path` into the current tab
static void recall_independent_view(AppState& app, const std::string& path) {
  if (!app.independent_dir_views) return;
  if (path == "computer://" || path == "trash://" ||
      path.rfind("recent://", 0) == 0)
    return;
  auto it = app.dir_view_states.find(path);
  if (it == app.dir_view_states.end()) return;
  auto& t = app.cur_tab();
  t.view_mode = it->second.view_mode;
  t.sort_field = it->second.sort_field;
  t.sort_descending = it->second.sort_descending;
  t.group_by_type = it->second.group_by_type;
  t.group_field = it->second.group_field;
  if (it->second.zoom_level >= 0 && it->second.zoom_level < kZoomLevelCount) {
    double zp = zoom_pct_for_level(it->second.zoom_level);
    if (std::abs(zp - app.zoom_pct) > 0.01) apply_zoom_pct(app, zp);
  }
  if (t.view_mode != ViewMode::Computer)
    app.last_browser_view_mode = t.view_mode;
}

void navigate_to(AppState& app, const std::string& path) {
  const bool startup_nav = app.startup_loading;
  auto nav_mark = [&](const char* what) {
    if (!startup_nav ||
        !trace::enabled().load(std::memory_order_relaxed))
      return;
    trace::log("STARTUP nav_%s", what);
  };
  struct NavClock {
    std::chrono::steady_clock::time_point t0;
    ~NavClock() {
      if (trace::enabled().load(std::memory_order_relaxed)) {
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        trace::log("STARTUP navigate_to_total %.1f ms", ms);
      }
    }
  } nav_clock{std::chrono::steady_clock::now()};
  (void)nav_mark;
  save_dir_props_before_leave(app);
  remember_independent_view(app);
  nav_mark("props_saved");
  // Handle virtual "computer://" path
  if (path == "computer://") {
    if (!app.cur_tab().current_path.empty() && app.cur_tab().current_path != path) {
      if (app.cur_tab().view_mode != ViewMode::Computer)
        app.last_browser_view_mode = app.cur_tab().view_mode;
      app.cur_tab().nav_history.push_back(app.cur_tab().current_path);
      app.cur_tab().nav_forward.clear();
    }
    app.cur_tab().current_path = path;
    app.cur_tab().view_mode = ViewMode::Computer;
    app.cur_tab().selected_idx = -1;
    app.cur_tab().hover_idx = -1;
    app.cur_tab().scroll_px = 0;
    app.cur_tab().scroll_smooth_current = 0;
    app.cur_tab().scroll_smooth_target = 0;
    app.computer_scroll_px = 0;
    app.computer_scroll_smooth_current = 0;
    app.computer_scroll_smooth_target = 0;
    app.computer_needs_refresh = true;
    draw(app);
    return;
  }

  std::string resolved = fs::absolute(path).lexically_normal().string();
  if (!fs::is_directory(resolved)) return;

  // Push current folder onto the back stack before leaving
  if (!app.cur_tab().current_path.empty() && app.cur_tab().current_path != resolved) {
    app.cur_tab().nav_history.push_back(app.cur_tab().current_path);
    app.cur_tab().nav_forward.clear();
  }
  app.cur_tab().current_path = resolved;
  app.cur_tab().selected_idx = -1;
  app.cur_tab().hover_idx = -1;
  app.cur_tab().scroll_px = 0;
  app.cur_tab().scroll_smooth_current = 0;
  app.cur_tab().scroll_smooth_target = 0;

  // Independent views per directory: restore this folder's remembered view
  recall_independent_view(app, resolved);

  // View/sort settings are global — keep the current ones across folders.
  // Only recover if we were in the virtual computer view.
  if (app.cur_tab().view_mode == ViewMode::Computer) {
    app.cur_tab().view_mode = app.last_browser_view_mode;
  }

  nav_mark("pre_scan");
  reload_dir(app);
  nav_mark("post_reload");
  // During cold start the frame loop paints immediately after us; drawing
  // here would build the whole glyph cache before the window even maps.
  if (!startup_nav)
    draw(app);
}

void navigate_up(AppState& app) {
  if (app.cur_tab().current_path == "computer://") return;
  fs::path p(app.cur_tab().current_path);
  auto parent = p.parent_path();
  if (parent != p) {
    navigate_to(app, parent.string());
  }
}

bool can_navigate_up(AppState& app) {
  const auto& path = app.cur_tab().current_path;
  if (path.empty() || path == "computer://" || path == "trash://" ||
      path.rfind("recent://", 0) == 0)
    return false;
  fs::path p(path);
  auto parent = p.parent_path();
  return parent != p;
}

void navigate_back(AppState& app) {
  if (app.cur_tab().nav_history.empty()) return;
  save_dir_props_before_leave(app);
  remember_independent_view(app);
  app.cur_tab().nav_forward.push_back(app.cur_tab().current_path);
  app.cur_tab().current_path = app.cur_tab().nav_history.back();
  app.cur_tab().nav_history.pop_back();
  app.cur_tab().selected_idx = -1;
  app.cur_tab().scroll_px = 0;
  recall_independent_view(app, app.cur_tab().current_path);
  reload_dir(app);
  draw(app);
}

void navigate_forward(AppState& app) {
  if (app.cur_tab().nav_forward.empty()) return;
  save_dir_props_before_leave(app);
  remember_independent_view(app);
  app.cur_tab().nav_history.push_back(app.cur_tab().current_path);
  app.cur_tab().current_path = app.cur_tab().nav_forward.back();
  app.cur_tab().nav_forward.pop_back();
  app.cur_tab().selected_idx = -1;
  app.cur_tab().scroll_px = 0;
  recall_independent_view(app, app.cur_tab().current_path);
  reload_dir(app);
  draw(app);
}


// ── mount drive ──────────────────────────────────────────────────

void mount_drive(AppState& app, int sb_idx) {
  auto& loc = app.sidebar_locations[sb_idx];
  if (loc.kind != SidebarLocation::Kind::Drive || loc.is_mounted) return;
  if (loc.drive_id.empty()) return; // no UDisks2 object path — can't mount

  auto& udisks = drives::UDisks2DriveService::instance();
  {
    std::lock_guard<std::mutex> lock(app.mount_mtx);
    app.mount_pending_drive_id = loc.drive_id;
  }

  udisks.mount_async(loc.drive_id, [&app](bool ok) {
    std::lock_guard<std::mutex> lock(app.mount_mtx);
    if (app.mount_pending_drive_id.empty()) return;
    app.mount_success = ok;
    app.mount_result_drive_id = std::move(app.mount_pending_drive_id);
    app.mount_pending_drive_id.clear();
    app.mount_poll_wake.store(true, std::memory_order_release);
  });
}

// ── unmount drive ────────────────────────────────────────────────

void unmount_drive(AppState& app, int sb_idx) {
  auto& loc = app.sidebar_locations[sb_idx];
  if (loc.kind != SidebarLocation::Kind::Drive || !loc.is_mounted) return;
  if (loc.drive_id.empty()) return;

  auto& udisks = drives::UDisks2DriveService::instance();
  {
    std::lock_guard<std::mutex> lock(app.mount_mtx);
    app.unmount_pending_drive_id = loc.drive_id;
  }

  udisks.unmount_async(loc.drive_id, [&app](bool ok) {
    std::lock_guard<std::mutex> lock(app.mount_mtx);
    if (app.unmount_pending_drive_id.empty()) return;
    app.unmount_success = ok;
    app.unmount_result_drive_id = std::move(app.unmount_pending_drive_id);
    app.unmount_pending_drive_id.clear();
    app.mount_poll_wake.store(true, std::memory_order_release);
  });
}

// ── open files ───────────────────────────────────────────────────

void open_selected(AppState& app) {
  auto& tab = app.cur_tab();
  // Tree view: selected_idx is into tree_entries
  if (tab.view_mode == ViewMode::Tree) {
    if (tab.selected_idx < 0 || tab.selected_idx >= static_cast<int>(tab.tree_entries.size())) return;
    auto& te = tab.tree_entries[tab.selected_idx];
    std::error_code ec;
    if (fs::is_directory(te.path, ec)) {
      navigate_to(app, te.path);
    } else {
      xdg::open_path_in_default_application(te.path);
    }
    return;
  }
  if (tab.selected_idx < 0 ||
      tab.selected_idx >= static_cast<int>(tab.visible_entries.size()))
    return;

  int real_idx = tab.visible_entries[tab.selected_idx];
  if (real_idx < 0 || real_idx >= static_cast<int>(tab.entries.size())) return;

  auto& entry = app.cur_tab().entries[real_idx];
  if (entry.type == FileType::Archive) {
    pid_t pid = fork();
    if (pid == 0) {
      execlp("horizon-archive", "horizon-archive", entry.path.c_str(), nullptr);
      _exit(1);
    }
    return;
  }
  if (entry.is_dir) {
    navigate_to(app, entry.path);
  } else if (entry.type == FileType::Executable) {
    pid_t pid = fork();
    if (pid == 0) {
      setsid();
      execlp(entry.path.c_str(), entry.path.c_str(), nullptr);
      _exit(1);
    }
  } else {
    xdg::open_path_in_default_application(entry.path);
  }
}

// ── Tab management ───────────────────────────────────────────────

void new_tab(AppState& app) {
  int idx = static_cast<int>(app.tabs.size());
  app.tabs.emplace_back();
  auto& prev = app.cur_tab();
  auto& next = app.tabs.back();
  next.view_mode = prev.view_mode;
  next.sort_field = prev.sort_field;
  next.sort_descending = prev.sort_descending;
  next.current_path = home_dir();
  app.active_tab = idx;
  navigate_to(app, next.current_path);
}

void close_tab(AppState& app) {
  if (app.tabs.size() <= 1) return;
  int idx = app.active_tab;
  remember_closed_tab(app, app.tabs[idx]);
  app.tabs.erase(app.tabs.begin() + idx);
  if (idx >= static_cast<int>(app.tabs.size()))
    app.active_tab = static_cast<int>(app.tabs.size()) - 1;
  reload_dir(app);
}

// ── split pane ───────────────────────────────────────────────────

// Copy path + persistent view settings from `src` into `dst`, resetting all
// transient state (history, scroll, selection). Mirrors Dolphin's secondary
// view: same folder as the source view, clean navigation state.
static void adopt_tab_state(Tab& dst, const Tab& src) {
  dst.current_path = src.current_path;
  dst.view_mode = src.view_mode;
  dst.sort_field = src.sort_field;
  dst.sort_descending = src.sort_descending;
  dst.group_by_type = src.group_by_type;
  dst.group_field = src.group_field;
  dst.nav_history.clear();
  dst.nav_forward.clear();
  dst.selected_idx = -1;
  dst.hover_idx = -1;
  dst.sel_anchor = -1;
  dst.multi_selected.clear();
  dst.scroll_px = 0;
  dst.scroll_smooth_current = 0;
  dst.scroll_smooth_target = 0;
}

void enter_split_view(AppState& app, int src_tab_idx,
                      const std::string& target_dir) {
  if (app.split_view) return;
  app.split_view = true;
  if (app.split_divider_x <= 0) app.split_divider_x = app.width / 2;
  int src = src_tab_idx;
  if (src < 0 || src >= static_cast<int>(app.tabs.size()))
    src = app.active_tab;
  adopt_tab_state(app.right_pane, app.tabs[src]);
  if (!target_dir.empty()) app.right_pane.current_path = target_dir;
  // Activate the newly created pane (Dolphin behavior)
  app.active_pane = 1;
  reload_dir(app);
}

void exit_split_view(AppState& app) {
  if (!app.split_view) return;
  if (app.active_pane == 1) {
    // Right pane is active: it survives and becomes the single view,
    // replacing the left pane's folder ("close active view").
    ViewMode vm = app.right_pane.view_mode;
    app.tabs[app.active_tab] = std::move(app.right_pane);
    app.right_pane = Tab{};
    if (vm != ViewMode::Computer) app.last_browser_view_mode = vm;
  } else {
    app.right_pane = Tab{};
  }
  app.split_view = false;
  app.active_pane = 0;
}

void sync_split_panes(AppState& app) {
  if (!app.split_view || app.tabs.empty()) return;
  adopt_tab_state(app.right_pane, app.tabs[app.active_tab]);
  reload_dir(app);
}

void open_tab_in_active_pane(AppState& app, int tab_idx) {
  if (tab_idx < 0 || tab_idx >= static_cast<int>(app.tabs.size())) return;
  if (!app.split_view || app.active_pane == 0) {
    app.active_tab = tab_idx;
    reload_dir(app);
    return;
  }
  adopt_tab_state(app.right_pane, app.tabs[tab_idx]);
  reload_dir(app);
}

void next_tab(AppState& app) {
  if (app.tabs.size() <= 1) return;
  app.active_tab = (app.active_tab + 1) % static_cast<int>(app.tabs.size());
  reload_dir(app);
}

void prev_tab(AppState& app) {
  if (app.tabs.size() <= 1) return;
  app.active_tab = (app.active_tab - 1 + static_cast<int>(app.tabs.size())) %
                   static_cast<int>(app.tabs.size());
  reload_dir(app);
}

} // namespace eh::file_browser

