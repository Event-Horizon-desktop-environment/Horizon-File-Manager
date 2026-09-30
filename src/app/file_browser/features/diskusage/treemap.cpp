// treemap.cpp — squarified treemap over the current view's top items,
// plus extension ranking colors and the trash action.

#include "app/file_browser/features/diskusage/diskusage.hpp"

#include "../../app.hpp"

#include "platform/desktop/entries/desktop_xdg_ops.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>

namespace eh::file_browser {
namespace xdg = eh::shell::desktop::xdg;
namespace dudetail {

// One layout input: a file or a directory aggregate.
struct Cell {
  std::string path;
  uint64_t size = 0;
  double r = 0.5, g = 0.5, b = 0.5;
  bool is_dir = false;
};

// Squarified treemap (Bruls et al.): rows split along the short side,
// minimizing the worst aspect ratio. Cells arrive size-descending.
void squarify_row(std::vector<DiskTile>& out,
                  const std::vector<dudetail::Cell>& cells, size_t from, size_t to,
                  double x, double y, double w, double h, bool horizontal,
                  const std::string& dir_prefix) {
  (void)dir_prefix;
  if (from >= to || w <= 0 || h <= 0) return;
  uint64_t total = 0;
  for (size_t i = from; i < to; ++i) total += cells[i].size;
  if (total == 0) return;
  double acc = 0;
  for (size_t i = from; i < to; ++i) {
    double frac = static_cast<double>(cells[i].size) /
                  static_cast<double>(total);
    if (horizontal) {
      double cw = w * frac;
      DiskTile t;
      t.path = cells[i].path;
      t.x = x + acc;
      t.y = y;
      t.w = cw;
      t.h = h;
      t.r = cells[i].r;
      t.g = cells[i].g;
      t.b = cells[i].b;
      t.size = cells[i].size;
      t.is_dir = cells[i].is_dir;
      out.push_back(std::move(t));
      acc += cw;
    } else {
      double ch = h * frac;
      DiskTile t;
      t.path = cells[i].path;
      t.x = x;
      t.y = y + acc;
      t.w = w;
      t.h = ch;
      t.r = cells[i].r;
      t.g = cells[i].g;
      t.b = cells[i].b;
      t.size = cells[i].size;
      t.is_dir = cells[i].is_dir;
      out.push_back(std::move(t));
      acc += ch;
    }
  }
}

double worst_aspect(const std::vector<dudetail::Cell>& cells, size_t from, size_t to,
                    double row_bytes, double fixed_side, double px_per_byte) {
  // Bruls et al. worst ratio, all in px²: fixed_side is the strip's FULL
  // side (the rect's long side), row area comes from bytes×scale. Byte
  // magnitudes must be scaled first — mixing raw bytes with pixel lengths
  // silently breaks row-splitting whenever bytes >> px² (every drive).
  if (row_bytes <= 0 || fixed_side <= 0 || px_per_byte <= 0)
    return std::numeric_limits<double>::infinity();
  double mx = 0, mn = std::numeric_limits<double>::infinity();
  for (size_t i = from; i < to; ++i) {
    double s = static_cast<double>(cells[i].size) * px_per_byte;
    if (s <= 0) continue;
    mx = std::max(mx, s);
    mn = std::min(mn, s);
  }
  if (!(mn > 0) || !(mx > 0))
    return std::numeric_limits<double>::infinity();
  const double s2 = fixed_side * fixed_side;
  const double row_px = row_bytes * px_per_byte;
  const double r2 = row_px * row_px;
  return std::max(s2 * mx / r2, r2 / (s2 * mn));
}

void squarify(std::vector<DiskTile>& out, const std::vector<dudetail::Cell>& cells,
              double x, double y, double w, double h) {
  uint64_t grand = 0;
  for (auto& c : cells) grand += c.size;
  size_t i = 0;
  const size_t n = cells.size();
  double cx = x, cy = y, cw = w, ch = h;
  // Remaining bytes back the current rect: each row takes row/remaining
  // (NOT row/grand — the rect shrinks as rows land, and dividing by the
  // constant grand crushes every row after the first into slivers while
  // leaving the tail unfilled).
  uint64_t remaining = grand;
  while (i < n) {
    bool horizontal = cw >= ch;
    // Strips span the LONG side and stack along the short one: a wide
    // rect gets full-width rows (cells across), a tall rect full-height
    // columns. (Inverted here once: full-height strips in a wide rect can
    // never square small cells — every level degenerated to 1-cell rows
    // at real byte magnitudes.)
    double fixed_side = horizontal ? cw : ch;
    // px² per byte for the CURRENT rect: keeps the aspect math in one
    // unit system (see worst_aspect).
    double px_per_byte =
        (remaining > 0 && cw > 0 && ch > 0)
            ? (cw * ch) / static_cast<double>(remaining)
            : 0.0;
    // Grow the row while the worst aspect improves.
    size_t best = i + 1;
    uint64_t row_bytes = cells[i].size;
    double best_worst =
        worst_aspect(cells, i, best, row_bytes, fixed_side, px_per_byte);
    while (best < n) {
      double cand = worst_aspect(cells, i, best + 1,
                                 row_bytes + cells[best].size, fixed_side,
                                 px_per_byte);
      if (cand >= best_worst) break;
      best_worst = cand;
      row_bytes += cells[best].size;
      ++best;
    }
    double frac = remaining > 0 ? static_cast<double>(row_bytes) /
                                        static_cast<double>(remaining)
                                  : 0;
    if (horizontal) {
      double rh = ch * frac;
      squarify_row(out, cells, i, best, cx, cy, cw, rh, true, {});
      cy += rh;
      ch -= rh;
    } else {
      double rw = cw * frac;
      squarify_row(out, cells, i, best, cx, cy, rw, ch, false, {});
      cx += rw;
      cw -= rw;
    }
    remaining -= row_bytes;
    i = best;
  }
}

} // namespace dudetail

// Lay ordered tiles out into a rect (paint-time geometry).
void layout_tiles(std::vector<DiskTile>& tiles, double x, double y, double w,
                  double h) {
  std::vector<dudetail::Cell> cells;
  cells.reserve(tiles.size());
  for (auto& t : tiles) {
    dudetail::Cell c;
    c.path = t.path;
    c.size = t.size;
    c.r = t.r;
    c.g = t.g;
    c.b = t.b;
    c.is_dir = t.is_dir;
    cells.push_back(std::move(c));
  }
  std::vector<DiskTile> laid;
  dudetail::squarify(laid, cells, x, y, w, h);
  tiles.swap(laid);
}

namespace {

// Path keys for the children index: no trailing slash ("/" stays "/").
std::string du_norm_key(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  if (p.empty()) p = "/";
  return p;
}

constexpr int kDuNestDepth = 8;
constexpr double kDuNestMin = 32.0; // min tile dim worth subdividing
constexpr double kDuFramePad = 3.0; // parent frame revealed around children
// Levels wider than this are truncated (pathological single dirs); the
// global cap below handles total size. Without this one 100k-entry dir
// could starve the whole map.
constexpr size_t kDuLevelCap = 5000;

// Breadth-first subdivision task: lay this parent's kids later, so every
// top-level child gets depth before any subtree goes deep (a depth-first
// walk spends the whole budget down the first big folder and leaves dark
// voids elsewhere).
struct NestTask {
  std::string parent; // index key (owned)
  size_t tile_idx;    // out[] index of the parent tile (for framed flag)
  double x, y, w, h;
  int depth;
};

// Collapse single-child chains (a/b/c with nothing else becomes c):
// deep library trees would otherwise spend the whole tile budget on
// frames before reaching a single file. Bytes are identical down the
// chain (a lone child owns its parent's whole subtree), so the tile is
// exact; the skipped levels stay reachable via the directory list.
const MapKid* collapse_chain(
    const std::unordered_map<std::string, std::vector<MapKid>>& index,
    const MapKid* k) {
  while (k->is_dir) {
    auto it = index.find(std::string(k->path));
    if (it == index.end() || it->second.size() != 1) break;
    k = &it->second.front();
  }
  return k;
}

// Squarify one pre-built kid list, then recurse into subdividable dirs.
// Cells copy (only painted tiles); the index itself holds views.
void layout_cells(std::vector<DiskTile>& out,
                  const std::unordered_map<std::string, std::vector<MapKid>>& index,
                  const std::vector<MapKid>& kids, const MapKid* tail,
                  double x, double y, double w, double h, int depth,
                  std::vector<NestTask>& tasks) {
  if ((kids.empty() && !tail) || w <= 0 || h <= 0) return;
  std::vector<dudetail::Cell> cells;
  cells.reserve(kids.size() + 1);
  std::vector<const MapKid*> collapsed;
  collapsed.reserve(kids.size());
  for (auto& k : kids) collapsed.push_back(collapse_chain(index, &k));
  // Pathological breadth guard: one 100k-entry dir must not starve the
  // map (dropped area underfills slightly; vanishingly rare).
  size_t take = collapsed.size();
  if (take > kDuLevelCap) take = kDuLevelCap;
  for (size_t ci = 0; ci < take; ++ci) {
    const MapKid* k = collapsed[ci];
    dudetail::Cell c;
    c.path = std::string(k->path);
    c.size = k->size;
    c.r = k->r;
    c.g = k->g;
    c.b = k->b;
    c.is_dir = k->is_dir;
    cells.push_back(std::move(c));
  }
  if (tail) {
    dudetail::Cell c;
    c.path = std::string(tail->path);
    c.size = tail->size;
    c.r = tail->r;
    c.g = tail->g;
    c.b = tail->b;
    c.is_dir = false;
    cells.push_back(std::move(c));
  }
  std::vector<DiskTile> laid;
  dudetail::squarify(laid, cells, x, y, w, h);
  for (auto& t : laid) {
    if (static_cast<int>(out.size()) >= kDuMaxTiles) return;
    t.depth = depth;
    // Parents first (paint + hit order); contents queue for later so the
    // whole level lands before anything goes deeper (breadth-first: a
    // depth-first walk spends the budget down the first big folder and
    // leaves dark voids elsewhere).
    size_t self = out.size();
    t.framed = false;
    out.push_back(t);
    if (t.is_dir && depth < kDuNestDepth && t.w >= kDuNestMin &&
        t.h >= kDuNestMin) {
      auto kit = index.find(t.path);
      if (kit != index.end() && !kit->second.empty()) {
        NestTask task;
        task.parent = t.path;
        task.tile_idx = self;
        task.x = t.x + kDuFramePad;
        task.y = t.y + kDuFramePad;
        task.w = t.w - 2 * kDuFramePad;
        task.h = t.h - 2 * kDuFramePad;
        task.depth = depth + 1;
        tasks.push_back(std::move(task));
      }
    }
  }
}

} // namespace

// Rebuild the cached children index (views into du.files/du.dirs —
// no path copies). Zoom-independent: every file/dir lands under its
// parent key once; layout starts wherever the zoom root is.
void du_build_index(AppState& app) {
  auto& du = app.diskusage;
  du.du_index.clear();
  if (du.files.empty()) {
    du.du_index_gen = du.du_data_gen;
    return;
  }
  // Extension -> color, shared single-source with the legend dots.
  std::unordered_map<std::string, std::array<double, 3>> colors;
  for (auto& e : du.exts) colors[e.ext] = {e.r, e.g, e.b};
  auto& index = du.du_index;
  index.reserve(du.dirs.size() + 1024);
  for (auto& f : du.files) {
    if (f.size == 0) continue;
    auto slash = f.path.rfind('/');
    std::string parent =
        (slash == std::string::npos) ? "/" : f.path.substr(0, slash);
    parent = du_norm_key(parent);
    auto it = colors.find(f.ext);
    MapKid k;
    k.path = f.path;
    k.size = f.size;
    k.is_dir = false;
    if (it != colors.end()) {
      k.r = it->second[0];
      k.g = it->second[1];
      k.b = it->second[2];
    }
    index[parent].push_back(k);
  }
  for (auto& [dpath, agg] : du.dirs) {
    if (agg.bytes == 0) continue;
    std::string dn = du_norm_key(dpath);
    auto slash = dn.rfind('/');
    std::string parent =
        (slash == std::string::npos || slash == 0) ? "/" : dn.substr(0, slash);
    MapKid k;
    k.path = dpath;
    k.size = agg.bytes;
    k.is_dir = true;
    // Frames are structural, never data: neutral gray always. Color means
    // file type — a folder must never invent its own hue, it subdivides
    // into its children's colors instead.
    k.r = k.g = k.b = 0.45;
    index[parent].push_back(k);
  }
  // Size-descending buckets (layout preserves this order).
  for (auto& [parent, vec] : index) {
    std::sort(vec.begin(), vec.end(),
              [](const MapKid& a, const MapKid& b) { return a.size > b.size; });
  }
  du.du_index_gen = du.du_data_gen;
}

// Lay the zoomed subtree into an explicit rect and record it.
// Skipped while a scan owns the vectors (paint keeps old tiles then).
// The index rebuild is the O(n) step and runs only when data changed;
// resize replays just the cheap squarify walk.
void du_layout_map(AppState& app, double x, double y, double w,
                   double h) {
  auto& du = app.diskusage;
  if (du.scanning.load(std::memory_order_relaxed)) return;
  if (w <= 0 || h <= 0) return;
  if (du.du_index_gen != du.du_data_gen) du_build_index(app);
  std::string zoom = du.treemap_root.empty() ? du.root : du.treemap_root;
  zoom = du_norm_key(zoom);
  // A highlight pointing at a vanished extension (e.g. its files were just
  // trashed away) would dim the whole map with no way back — clear it.
  if (!du.highlight_ext.empty()) {
    bool known = false;
    for (auto& e : du.exts) {
      if (e.ext == du.highlight_ext) {
        known = true;
        break;
      }
    }
    if (!known) du.highlight_ext.clear();
  }
  du.tiles.clear();
  du.hover_tile = -1;
  auto it = du.du_index.find(zoom);
  if (it == du.du_index.end() || it->second.empty()) return;
  // Top-level tiny-tile cutoff (QDirStat-style): roll the long tail into
  // one "<smaller>" bucket so the map stays readable.
  static constexpr size_t kTopTail = 600;
  const MapKid* tail = nullptr;
  MapKid tail_storage;
  std::vector<MapKid> top;
  if (it->second.size() > kTopTail) {
    uint64_t tail_bytes = 0;
    for (size_t i = kTopTail; i < it->second.size(); ++i)
      tail_bytes += it->second[i].size;
    top.assign(it->second.begin(), it->second.begin() + kTopTail);
    if (tail_bytes > 0) {
      du.du_tail_path = zoom + "/<smaller>";
      tail_storage.path = du.du_tail_path;
      tail_storage.size = tail_bytes;
      tail_storage.is_dir = false;
      tail_storage.r = tail_storage.g = tail_storage.b = 0.35;
      tail = &tail_storage;
    }
  }
  const std::vector<MapKid>& kids =
      (it->second.size() > kTopTail) ? top : it->second;
  // Breadth-first drain: FIFO over strictly deepening tasks lays each
  // level everywhere before descending, so the cap spreads evenly instead
  // of vanishing down the first big subtree.
  std::vector<NestTask> tasks;
  layout_cells(du.tiles, du.du_index, kids, tail, x, y, w, h, 0, tasks);
  for (size_t qi = 0;
       qi < tasks.size() &&
       static_cast<int>(du.tiles.size()) < kDuMaxTiles;
       ++qi) {
    // Copy out: laying out below pushes more tasks (reallocation).
    const std::string tparent = tasks[qi].parent;
    const size_t ttile = tasks[qi].tile_idx;
    const double tx = tasks[qi].x, ty = tasks[qi].y;
    const double tw = tasks[qi].w, th = tasks[qi].h;
    const int tdepth = tasks[qi].depth;
    auto kit = du.du_index.find(tparent);
    if (kit == du.du_index.end() || kit->second.empty()) continue;
    size_t before = du.tiles.size();
    layout_cells(du.tiles, du.du_index, kit->second, nullptr, tx, ty, tw,
                 th, tdepth, tasks);
    // Framed only when at least one child is paint-visible: an unfilled
    // frame with invisible (sub-pixel) children is just a dark void, so
    // those fall back to the neutral gray fill instead.
    bool visible_kid = false;
    for (size_t vi = before; vi < du.tiles.size(); ++vi) {
      if (du.tiles[vi].w > 2.5 && du.tiles[vi].h > 2.5) {
        visible_kid = true;
        break;
      }
    }
    if (ttile < du.tiles.size())
      du.tiles[ttile].framed = visible_kid;
  }
  du.laid_map[0] = static_cast<int>(x);
  du.laid_map[1] = static_cast<int>(y);
  du.laid_map[2] = static_cast<int>(w);
  du.laid_map[3] = static_cast<int>(h);
}

void du_refresh_views(AppState& app) {
  du_layout_map(app, kDuMapX, kDuMapY, kDuMapW, kDuMapH);
}

// True when the highlight names an extension with at least one
// file tile in the current view: otherwise dimming would wash
// the whole map with no visible match (stuck highlight).
bool du_highlight_live(AppState& app) {
  auto& du = app.diskusage;
  if (du.highlight_ext.empty() || du.tiles.empty()) return false;
  for (auto& e : du.exts) {
    if (e.ext != du.highlight_ext) continue;
    for (auto& t : du.tiles) {
      if (!t.is_dir && std::abs(e.r - t.r) < 0.01 &&
          std::abs(e.g - t.g) < 0.01 && std::abs(e.b - t.b) < 0.01)
        return true;
    }
    return false;
  }
  return false;
}

bool du_trash_selected(AppState& app, std::string& err) {
  auto& du = app.diskusage;
  if (du.selected.empty() || du.selected == du.root) {
    err = "Nothing to delete";
    return false;
  }
  // Never trash outside the scanned root.
  std::string base = du.root;
  if (!base.empty() && base.back() != '/') base += '/';
  if (du.selected != du.root &&
      du.selected.compare(0, base.size(), base) != 0) {
    err = "Outside the scanned drive";
    return false;
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  uint64_t freed = 0;
  bool is_dir = fs::is_directory(du.selected, ec);
  if (!ec) {
    if (is_dir) {
      auto it = du.dirs.find(du.selected);
      if (it != du.dirs.end()) freed = it->second.bytes;
    } else {
      // Exact file size from the sorted records.
      DiskFile key{du.selected, 0, {}};
      auto it = std::lower_bound(
          du.files.begin(), du.files.end(), key,
          [](const DiskFile& a, const DiskFile& b) { return a.path < b.path; });
      if (it != du.files.end() && it->path == du.selected)
        freed = it->size;
    }
  }
  // XDG trash (same backend as the main window's Move to Trash).
  if (!xdg::trash_file(du.selected)) {
    err = "Trash failed";
    return false;
  }
  // Evict from the model without a full rescan: drop the file record (or
  // the whole subtree), subtract along ancestor chains, refresh views.
  if (is_dir) {
    std::string prefix = du.selected;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';
    du.files.erase(
        std::remove_if(du.files.begin(), du.files.end(),
                       [&](const DiskFile& f) {
                         return f.path == du.selected ||
                                f.path.compare(0, prefix.size(), prefix) == 0;
                       }),
        du.files.end());
    for (auto it = du.dirs.begin(); it != du.dirs.end();) {
      if (it->first == du.selected ||
          it->first.compare(0, prefix.size(), prefix) == 0)
        it = du.dirs.erase(it);
      else
        ++it;
    }
    for (auto& [dpath, agg] : du.dirs) {
      if (dpath.size() >= du.selected.size()) continue;
      if (du.selected.compare(0, dpath.size(), dpath) == 0 &&
          (dpath.size() == du.selected.size() ||
           du.selected[dpath.size()] == '/')) {
        agg.bytes -= std::min(agg.bytes, freed);
        // counts recomputed lazily on rescan; decrement files only.
      }
    }
  } else {
    DiskFile key{du.selected, 0, {}};
    auto it = std::lower_bound(
        du.files.begin(), du.files.end(), key,
        [](const DiskFile& a, const DiskFile& b) { return a.path < b.path; });
    if (it != du.files.end() && it->path == du.selected)
      du.files.erase(it);
    for (auto& [dpath, agg] : du.dirs) {
      if (dpath.size() >= du.selected.size()) continue;
      if (du.selected.compare(0, dpath.size(), dpath) == 0 &&
          (dpath.size() == du.selected.size() ||
           du.selected[dpath.size()] == '/')) {
        agg.bytes -= std::min(agg.bytes, freed);
        if (agg.files > 0) agg.files -= 1;
      }
    }
  }
  // Extension aggregates recomputed from the surviving files.
  {
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> m;
    for (auto& f : du.files) {
      auto& e = m[f.ext];
      e.first += f.size;
      e.second += 1;
    }
    // Keep existing colors by extension.
    std::unordered_map<std::string, std::array<double, 3>> colors;
    for (auto& e : du.exts) colors[e.ext] = {e.r, e.g, e.b};
    du.exts.clear();
    for (auto& [ext, v] : m) {
      DiskExt e;
      e.ext = ext;
      e.bytes = v.first;
      e.files = v.second;
      auto it = colors.find(ext);
      if (it != colors.end()) {
        e.r = it->second[0];
        e.g = it->second[1];
        e.b = it->second[2];
      }
      du.exts.push_back(std::move(e));
    }
    std::sort(du.exts.begin(), du.exts.end(),
              [](const DiskExt& a, const DiskExt& b) {
                return a.bytes > b.bytes;
              });
  }
  du.total_bytes -= std::min(du.total_bytes, freed);
  du.selected.clear();
  du.du_data_gen++; // files/dirs mutated above; cached index views dangle
  du_refresh_views(app);
  return true;
}

} // namespace eh::file_browser
