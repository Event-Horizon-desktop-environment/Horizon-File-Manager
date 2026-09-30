// duplicates.cpp — duplicate finder for the disk usage window.
//
// Pipeline: size buckets (skip empties) -> inode-identity collapse
// (hardlinks are one file, never "duplicates") -> partial FNV-1a-64 over
// the first 64 KiB -> full FNV-1a-64 -> groups of ≥2 distinct inodes.
// Single sequential worker: hashing is streaming-IO bound and sequential
// reads stay cache-friendly. UI updates via DeferredCall.
//
// Protocol (data races): the worker reads du.files directly, so no model
// mutation may run while it lives. Every mutating site (single trash,
// group trash completion, rescan, drive switch, close) stops it first via
// du_stop_dup_scan (joinable, never detached).

#include "app/file_browser/features/diskusage/diskusage.hpp"

#include "../../app.hpp"

#include "base/thread/thread_dispatch.hpp"
#include "platform/desktop/entries/desktop_xdg_ops.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace eh::file_browser {
namespace xdg = eh::shell::desktop::xdg;
namespace {

constexpr size_t kPartialBytes = 64 * 1024;
constexpr size_t kReadChunk = 256 * 1024;

// FNV-1a 64 over up to `limit` bytes (0 = whole file). Sets ok=false on
// any IO error; checks cancel per chunk.
uint64_t fnv_file(const std::string& path, uint64_t limit,
                  const std::atomic<bool>& cancel, bool& ok) {
  ok = false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return 0;
  uint64_t h = 14695981039346656037ULL;
  std::vector<char> buf(kReadChunk);
  uint64_t left = (limit == 0) ? UINT64_MAX : limit;
  while (left > 0 && !cancel.load(std::memory_order_relaxed)) {
    size_t want = buf.size();
    if ((uint64_t)want > left) want = static_cast<size_t>(left);
    size_t got = std::fread(buf.data(), 1, want, f);
    if (got == 0) {
      if (std::feof(f)) break;
      std::fclose(f);
      return 0; // read error
    }
    for (size_t i = 0; i < got; ++i) {
      h ^= static_cast<unsigned char>(buf[i]);
      h *= 1099511628211ULL;
    }
    left -= got;
    if (got < want) break; // EOF
  }
  std::fclose(f);
  if (cancel.load(std::memory_order_relaxed)) return 0;
  ok = true;
  return h;
}

struct DevIno {
  uint64_t dev = 0;
  uint64_t ino = 0;
};

} // namespace

void du_rebuild_dup_rows(AppState& app) {
  auto& du = app.diskusage;
  du.dup_rows.clear();
  du.dup_wasted = 0;
  for (auto& g : du.dup_groups)
    du.dup_wasted += (g.paths.size() - 1) * g.size;
  static constexpr size_t kRowCap = 2000;
  for (size_t g = 0; g < du.dup_groups.size(); ++g) {
    if (du.dup_rows.size() >= kRowCap) break;
    DuRow h;
    h.header = true;
    h.group = g;
    du.dup_rows.push_back(h);
    for (size_t m = 0; m < du.dup_groups[g].paths.size(); ++m) {
      if (du.dup_rows.size() >= kRowCap) break;
      DuRow r;
      r.header = false;
      r.group = g;
      r.member = m;
      du.dup_rows.push_back(r);
    }
  }
  du.dup_content_h = static_cast<int>(du.dup_rows.size()) * 26;
}

void du_stop_dup_scan(AppState& app) {
  auto& du = app.diskusage;
  du.dup_cancel.store(true);
  du.dup_scanning.store(false);
  if (du.dup_thread.joinable()) du.dup_thread.join();
}

void du_start_dup_scan(AppState& app) {
  auto& du = app.diskusage;
  du_stop_dup_scan(app);
  du.dup_groups.clear();
  du.dup_rows.clear();
  du.dup_done = false;
  du.dup_skipped = 0;
  du.dup_confirm_armed = false;
  du.dup_scroll = 0;
  if (!du.done || du.files.empty()) return; // nothing to hash yet
  du.dup_cancel.store(false);
  du.dup_scanning.store(true);
  du.dup_p_done.store(0);
  du.dup_p_total.store(0);

  AppState* ap = &app;
  du.dup_thread = std::thread([ap]() {
    auto& du = ap->diskusage;
    auto cancelled = [&] {
      return du.dup_cancel.load(std::memory_order_relaxed);
    };
    // 1. Size buckets (empties can never waste space: skip).
    std::unordered_map<uint64_t, std::vector<size_t>> by_size;
    by_size.reserve(du.files.size() / 4 + 1024);
    for (size_t i = 0; i < du.files.size(); ++i) {
      if (cancelled()) {
        du.dup_scanning.store(false);
        return;
      }
      if (du.files[i].size > 0) by_size[du.files[i].size].push_back(i);
    }
    uint64_t total = 0;
    for (auto& [sz, v] : by_size)
      if (v.size() > 1) total += v.size();
    du.dup_p_total.store(total, std::memory_order_relaxed);
    auto progress_tick = [&] {
      du.dup_p_done.fetch_add(1, std::memory_order_relaxed);
    };
    struct Cand {
      std::string path; // view into du.files (stable: no mutation live)
      uint64_t dev = 0;
      uint64_t ino = 0;
    };
    std::vector<DuGroup> groups;
    for (auto& [sz, idxs] : by_size) {
      if (idxs.size() < 2) continue;
      if (cancelled()) break;
      // 2. Identity collapse: same (dev, ino) is one file (hardlink).
      std::vector<Cand> cands;
      cands.reserve(idxs.size());
      for (size_t fi : idxs) {
        if (cancelled()) break;
        const std::string& path = du.files[fi].path;
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0 ||
            !S_ISREG(st.st_mode)) {
          du.dup_skipped.fetch_add(1, std::memory_order_relaxed);
          progress_tick();
          continue;
        }
        bool seen = false;
        for (auto& c : cands) {
          if (c.dev == static_cast<uint64_t>(st.st_dev) &&
              c.ino == static_cast<uint64_t>(st.st_ino)) {
            seen = true;
            break;
          }
        }
        if (!seen)
          cands.push_back(
              {path, static_cast<uint64_t>(st.st_dev),
               static_cast<uint64_t>(st.st_ino)});
      }
      if (cancelled()) break;
      if (cands.size() < 2) {
        for (size_t k = 0; k < idxs.size(); ++k) progress_tick();
        continue;
      }
      // 3. Partial hash prefilter.
      std::unordered_map<uint64_t, std::vector<Cand>> by_part;
      for (auto& c : cands) {
        if (cancelled()) break;
        bool ok = false;
        uint64_t h = fnv_file(c.path, kPartialBytes, du.dup_cancel, ok);
        if (!ok) {
          du.dup_skipped.fetch_add(1, std::memory_order_relaxed);
          progress_tick();
          continue;
        }
        by_part[h].push_back(std::move(c));
        progress_tick();
      }
      if (cancelled()) break;
      // 4. Full hash within partial groups.
      for (auto& [ph, vec] : by_part) {
        if (vec.size() < 2) continue;
        std::unordered_map<uint64_t, std::vector<std::string>> by_full;
        for (auto& c : vec) {
          if (cancelled()) break;
          bool ok = false;
          uint64_t h = fnv_file(c.path, 0, du.dup_cancel, ok);
          if (!ok) {
            du.dup_skipped.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          by_full[h].push_back(c.path);
        }
        if (cancelled()) break;
        for (auto& [fh, paths] : by_full) {
          if (paths.size() < 2) continue;
          std::sort(paths.begin(), paths.end());
          DuGroup g;
          g.size = sz;
          g.hash = fh;
          g.paths = std::move(paths);
          groups.push_back(std::move(g));
        }
      }
      if (cancelled()) break;
    }
    if (cancelled()) {
      du.dup_scanning.store(false);
      return;
    }
    // Most waste first.
    std::sort(groups.begin(), groups.end(), [](const DuGroup& a,
                                               const DuGroup& b) {
      uint64_t wa = (a.paths.size() - 1) * a.size;
      uint64_t wb = (b.paths.size() - 1) * b.size;
      return wa > wb;
    });
    uint64_t skipped = du.dup_skipped.load(std::memory_order_relaxed);
    du.dup_scanning.store(false);
    DeferredCall::callLater([ap, groups = std::move(groups), skipped]() mutable {
      auto& du = ap->diskusage;
      du.dup_groups = std::move(groups);
      du.dup_data_gen = du.du_data_gen;
      du.dup_skipped = skipped;
      du.dup_done = true;
      du_rebuild_dup_rows(*ap);
      if (ap->du_open) {
        ap->du_pendingRedraw = true;
        draw_diskusage_window(*ap);
      }
    });
  });
}


void du_evict_paths(AppState& app, const std::vector<std::string>& trashed) {
  auto& du = app.diskusage;
  if (trashed.empty()) return;
  std::unordered_set<std::string> gone(trashed.begin(), trashed.end());
  // Per-path sizes from the sorted records, then one erase pass.
  std::unordered_map<std::string, uint64_t> freed;
  uint64_t freed_total = 0;
  for (auto& p : trashed) {
    DiskFile key{p, 0, {}};
    auto it = std::lower_bound(
        du.files.begin(), du.files.end(), key,
        [](const DiskFile& a, const DiskFile& b) { return a.path < b.path; });
    if (it != du.files.end() && it->path == p) {
      freed[p] = it->size;
      freed_total += it->size;
    }
  }
  du.files.erase(
      std::remove_if(du.files.begin(), du.files.end(),
                     [&](const DiskFile& f) { return gone.count(f.path) > 0; }),
      du.files.end());
  // Subtract each file's own bytes along its ancestor chains.
  for (auto& [p, sz] : freed) {
    for (auto& [dpath, agg] : du.dirs) {
      if (dpath.size() >= p.size()) continue;
      if (p.compare(0, dpath.size(), dpath) == 0 &&
          (dpath.size() == p.size() || p[dpath.size()] == '/')) {
        agg.bytes -= std::min(agg.bytes, sz);
        if (agg.files > 0) agg.files -= 1;
      }
    }
  }
  // Extension aggregates recomputed once from survivors.
  {
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> m;
    for (auto& f : du.files) {
      auto& e = m[f.ext];
      e.first += f.size;
      e.second += 1;
    }
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
  du.total_bytes -= std::min(du.total_bytes, freed_total);
  du.total_files -= std::min<uint64_t>(du.total_files, trashed.size());
  du.du_data_gen++;
}


void du_stop_dup_trash(AppState& app) {
  auto& du = app.diskusage;
  du.dupact_cancel.store(true);
  du.dupact_running.store(false);
  if (du.dupact_thread.joinable()) du.dupact_thread.join();
}

void du_start_dup_trash(AppState& app) {
  auto& du = app.diskusage;
  du_stop_dup_trash(app);
  if (du.dup_groups.empty()) return;
  std::vector<std::string> targets;
  for (auto& g : du.dup_groups) {
    for (size_t i = 1; i < g.paths.size(); ++i)
      targets.push_back(g.paths[i]);
  }
  if (targets.empty()) return;
  du.dupact_cancel.store(false);
  du.dupact_running.store(true);
  du.dupact_done.store(0);
  du.dupact_total.store(targets.size());
  AppState* ap = &app;
  du.dupact_thread = std::thread([ap, targets = std::move(targets)]() mutable {
    auto& du = ap->diskusage;
    std::vector<std::string> ok;
    std::string first_err;
    for (auto& p : targets) {
      if (du.dupact_cancel.load(std::memory_order_relaxed)) break;
      if (xdg::trash_file(p))
        ok.push_back(p);
      else if (first_err.empty())
        first_err = p;
      du.dupact_done.fetch_add(1, std::memory_order_relaxed);
    }
    bool cancelled = du.dupact_cancel.load(std::memory_order_relaxed);
    du.dupact_running.store(false);
    DeferredCall::callLater(
        [ap, ok = std::move(ok), first_err, cancelled]() mutable {
          auto& du = ap->diskusage;
          if (!cancelled && !ok.empty()) {
            du_evict_paths(*ap, ok);
            du.selected.clear();
            du_rebuild_dup_rows(*ap);
            du_rebuild_kids(*ap);
            du_refresh_views(*ap);
            schedule_trash_maintain(*ap);
          }
          du.dup_confirm_armed = false;
          // Data changed: fresh duplicate scan over the survivors. Model
          // maintenance, not paint — runs headless too (draw stays gated).
          if (!cancelled && !ok.empty() && du.dup_mode)
            du_start_dup_scan(*ap);
          if (ap->du_open) {
            if (!ok.empty()) {
              char buf[128];
              snprintf(buf, sizeof(buf), "Trashed %zu duplicate%s",
                       ok.size(), ok.size() == 1 ? "" : "s");
              du.status_msg = buf;
              if (!first_err.empty()) {
                du.status_msg += " (1 failed: " + first_err + ")";
              }
              du.status_until_ms =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count() +
                  4000;
            } else if (!cancelled) {
              du.status_msg = first_err.empty() ? "Nothing trashed"
                                                : ("Trash failed: " + first_err);
              du.status_until_ms =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count() +
                  4000;
            }
            // Data changed: fresh scan scheduled above (model work).
            ap->du_pendingRedraw = true;
            if (ap->du_open) draw_diskusage_window(*ap);
          }
        });
  });
}

} // namespace eh::file_browser
