#pragma once

// diskusage.hpp — WinDirStat-style disk usage analyzer.

#include <cairo/cairo.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace eh::file_browser {

class AppState;

// One duplicate group: same size + same full hash, distinct inodes.
struct DuGroup {
  uint64_t size = 0; // bytes per file
  uint64_t hash = 0; // FNV-1a-64 over full content
  std::vector<std::string> paths; // ≥2, sorted
};

// One cached display row for the duplicates view (rebuilt on publish).
struct DuRow {
  bool header = false; // group header vs member file
  size_t group = 0;
  size_t member = 0; // member index within group (files only)
};

// One scanned file (path-sorted after the scan for drill-down ranges).
struct DiskFile {
  std::string path;
  uint64_t size = 0;
  std::string ext; // lowercase, no dot ("" if none)
};

// Per-directory aggregates (keyed by absolute path).
struct DiskDir {
  uint64_t bytes = 0;
  uint64_t files = 0;
  uint64_t dirs = 0; // immediate subdirs (for "N items" display)
};

// Extension aggregate for the legend (sorted desc at publish).
struct DiskExt {
  std::string ext; // "" = no extension
  uint64_t bytes = 0;
  uint64_t files = 0;
  double r = 0.5, g = 0.5, b = 0.5; // legend color (top ranks get palette)
};

// Treemap tile (current view only, recomputed on drill/zoom/rescan).
// Nested WinDirStat-style: directory tiles CONTAIN their children's tiles.
// Leaves (files, unsubdivided dirs) carry the area; parents are frames.
// File tiles use the extension-legend color (single source of truth);
// directory frames use their largest child's color.
struct MapKid {
  std::string_view path;
  uint64_t size = 0;
  bool is_dir = false;
  double r = 0.5, g = 0.5, b = 0.5;
};

struct DiskTile {
  std::string path;
  double x = 0, y = 0, w = 0, h = 0;
  double r = 0.5, g = 0.5, b = 0.5;
  uint64_t size = 0;
  bool is_dir = false;
  int depth = 0; // 0 = child of the zoom root
  bool framed = false; // subdivided dir: border + label only, never a fill
};

// Fixed window (1120x760, min==max): treemap content rect shared by the
// layout (treemap.cpp) and paint/hit-testing (window.cpp). Keep in sync
// with the panel geometry in draw_diskusage_dialog.
inline constexpr int kDuMapX = 22;
inline constexpr int kDuMapY = 508;
inline constexpr int kDuMapW = 1076;
inline constexpr int kDuMapH = 206;

// Hard cap on tiles (paint/hover/click cost, not hit IDs: tiles resolve
// by geometry search, so the cap is purely a perf guard). Sized for real
// drives: breadth-first layout spreads it evenly across the map.
inline constexpr int kDuMaxTiles = 20000;

struct DiskUsageState {
  std::string root; // scan root (drive mount path)
  // Scan worker state.
  std::thread scan_thread;
  std::atomic<bool> scanning{false};
  std::atomic<bool> cancel{false};
  std::atomic<uint64_t> p_files{0};
  std::atomic<uint64_t> p_dirs{0};
  std::atomic<uint64_t> p_bytes{0};
  std::atomic<uint64_t> p_errors{0};
  // Published results (UI thread; workers merge at join, progress rides
  // DeferredCall throttled to ~2Hz).
  bool done = false;
  uint64_t total_files = 0;
  uint64_t total_dirs = 0;
  uint64_t total_bytes = 0;
  uint64_t unknown_dirs = 0; // EACCES prunes (WinDirStat <Unknown>)
  uint64_t pruned_dirs = 0;  // virtual-FS prunes (/proc, /sys…), not errors
  std::vector<DiskFile> files; // path-sorted
  std::unordered_map<std::string, DiskDir> dirs;
  std::vector<DiskExt> exts; // bytes-desc
  // View state.
  std::string cur_dir;  // drill path (root at open)
  std::string selected; // synced across list/treemap/extensions
  std::string highlight_ext; // extension highlight from legend ("" = off)
  std::string treemap_root;  // zoom root (mirrors cur_dir in slice 1)
  std::vector<DiskTile> tiles;
  int scroll_dir = 0;
  int scroll_ext = 0;
  int hover_tile = -1;
  int hover_row = -1; // absolute index into dir_kids, -1 = none
  // Treemap layout bookkeeping (resizable window): rect the stored tiles
  // were laid out for. Paint re-lays when it moved (squarify only; the
  // children index is cached, so resize drags stay cheap).
  int laid_map[4]{};
  // Cached children index (MapKid views, no path copies): rebuilt when
  // du_data_gen != du_index_gen. Bumped on every files/dirs mutation
  // (scan clear/publish, trash evict) — all on the UI thread.
  uint64_t du_data_gen = 0;
  uint64_t du_index_gen = 0;
  std::unordered_map<std::string, std::vector<MapKid>> du_index;
  std::string du_tail_path; // owned storage for the "<smaller>" bucket view
  // Double-click drill detection.
  int64_t last_click_ms = 0;
  std::string last_click_path;
  // Status-bar message (action results), millis expiry.
  std::string status_msg;
  int64_t status_until_ms = 0;
  // Local settings panel (gear toggle): open flag, active slider drag
  // (0 = none, else kDuSliderBase+i), slider hit rects for drag math.
  bool settings_open = false;
  int slider_drag = 0;
  int slider_rect[5][4]{};
  // List scrollbar drags: 0 = none, 1 = directory panel, 2 = extension.
  int scroll_drag = 0;
  // Cached children of cur_dir (rebuilt on navigate/publish, not per frame).
  struct DuChild {
    std::string path;
    std::string name;
    uint64_t size = 0;
    bool is_dir = false;
  };
  std::vector<DuChild> dir_kids;
  int dir_content_h = 0;
  int ext_content_h = 0;
  int dir_rect[4]{}; // x, y, w, h of the directory list region
  int ext_rect[4]{}; // x, y, w, h of the extension list region
  int map_rect[4]{}; // x, y, w, h of the treemap region
  // Breadcrumb segments for the toolbar drill path (rebuilt per paint).
  std::vector<std::string> crumb_labels;
  std::vector<std::string> crumb_paths;
  // Duplicates view (toggle): hash-scan worker + grouped results.
  bool dup_mode = false;
  std::thread dup_thread;
  std::atomic<bool> dup_scanning{false};
  std::atomic<bool> dup_cancel{false};
  std::atomic<uint64_t> dup_p_done{0};
  std::atomic<uint64_t> dup_p_total{0};
  bool dup_done = false;
  std::atomic<uint64_t> dup_skipped{0}; // unreadable mid-hash (worker)
  uint64_t dup_wasted = 0; // total reclaimable bytes across groups
  uint64_t dup_data_gen = 0; // du_data_gen the results were built from
  std::vector<DuGroup> dup_groups; // wasted-desc
  std::vector<DuRow> dup_rows;     // cached display rows
  int dup_scroll = 0;
  int dup_content_h = 0;
  // Group-trash worker (keep first per group): two-step armed confirm.
  bool dup_confirm_armed = false;
  std::thread dupact_thread;
  std::atomic<bool> dupact_running{false};
  std::atomic<bool> dupact_cancel{false};
  std::atomic<uint64_t> dupact_done{0};
  std::atomic<uint64_t> dupact_total{0};
};

// Window lifecycle (mirror the settings window pattern).
void open_disk_usage(AppState& app, const std::string& root);
void close_disk_usage(AppState& app);
bool disk_usage_open(const AppState& app);
void draw_diskusage_window(AppState& app);
void draw_diskusage_dialog(AppState& app, cairo_t* cr);
void handle_du_click(AppState& app, int x, int y, int button);
void handle_du_axis(AppState& app, int x, int y, double dy);
bool handle_du_key(AppState& app, uint32_t sym, bool ctrl);

// Scan control (worker threads; UI updates via DeferredCall).
void du_start_scan(AppState& app);
void du_stop_scan(AppState& app);
// Duplicate finder (single hashing worker; UI updates via DeferredCall).
void du_start_dup_scan(AppState& app);
void du_stop_dup_scan(AppState& app);
void du_rebuild_dup_rows(AppState& app);
// Group trash (keep first per group) on a worker; progress via status.
void du_start_dup_trash(AppState& app);
void du_stop_dup_trash(AppState& app);
// Batch evict already-trashed file paths (single model update + refresh
// left to the caller). Files only; dirs never enter duplicate groups.
void du_evict_paths(AppState& app, const std::vector<std::string>& trashed);

// One layout input: path views into du.files/du.dirs (no copies), so the
// index is cheap to keep across drills and resizes. Valid only while
// du_index_gen == du_data_gen (bumped on every files/dirs mutation).
// Treemap tile geometry for the current ordered tiles (paint time).
void layout_tiles(std::vector<DiskTile>& tiles, double x, double y, double w,
                  double h);

// Rebuild derived views (extension ranking, treemap) for the current
// drill/zoom state. UI thread; call after publish and on navigation.
void du_refresh_views(AppState& app);
// Lay the zoomed subtree into an explicit rect (resize path). Records the
// rect + timestamp in laid_map/last_layout_ms.
void du_layout_map(AppState& app, double x, double y, double w, double h);
// True when the highlight names an extension with a file tile in view.
bool du_highlight_live(AppState& app);
// CSV report: summary + directories + extensions + duplicates tables.
// Pure (no IO) so probes can verify the exact bytes.
std::string du_build_report_csv(const AppState& app);
// Breadcrumb segments (label + drill path) for the toolbar. Pure.
void du_build_crumbs(AppState& app);
// Local settings panel: apply slider value from an x coordinate (press or
// drag), and finish a drag (persists to disk-usage.toml).
void du_apply_slider_at(AppState& app, int ctrl, int x);
void du_end_slider_drag(AppState& app);
// List scrollbar drag: recompute scroll offset from a y coordinate.
void du_apply_scroll_drag(AppState& app, int y);
// Rebuild the cached children of cur_dir. UI thread.
void du_rebuild_kids(AppState& app);

// Extension for treemap/legend coloring (lowercase, no dot).
std::string du_file_ext(const std::string& name);
// Virtual-FS pruning (tested headlessly; see scan.cpp for the lists).
bool du_prune_dir_path(const std::string& full);
bool du_is_pseudo_fs(const std::string& path);

// Actions.
bool du_trash_selected(AppState& app, std::string& err);

} // namespace eh::file_browser
