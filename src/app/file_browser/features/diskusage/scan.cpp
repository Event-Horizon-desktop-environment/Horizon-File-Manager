// scan.cpp — parallel size walker for the disk usage window.
//
// Workers pull directories off a shared stack (no per-file locking).
// Files are collected with (dev, ino) so hardlinks dedupe at merge;
// symlinks are never followed; EACCES dirs become <Unknown> counts.
// Aggregation (dir tree, extensions) runs single-threaded at join over
// the path-sorted file list.

#include "app/file_browser/features/diskusage/diskusage.hpp"

#include "../../app.hpp"

#include "base/thread/thread_dispatch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

namespace eh::file_browser {
namespace {

struct DevIno {
  uint64_t dev = 0;
  uint64_t ino = 0;
  bool operator==(const DevIno& o) const {
    return dev == o.dev && ino == o.ino;
  }
};
struct DevInoHash {
  size_t operator()(const DevIno& k) const noexcept {
    size_t h = std::hash<uint64_t>{}(k.dev * 1000000007ULL + k.ino);
    return h ^ (h >> 29);
  }
};

struct RawFile {
  std::string path;
  uint64_t size = 0;
  uint64_t dev = 0;
  uint64_t ino = 0;
};

struct ScanShared {
  std::mutex mtx;
  std::vector<std::string> dir_stack; // work items (absolute paths)
  std::atomic<size_t> pending{0};     // dirs queued or being walked
  std::atomic<bool> done{false};
  std::atomic<uint64_t> files{0};
  std::atomic<uint64_t> dirs{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> pruned{0}; // virtual-FS dirs skipped, not errors
  std::atomic<bool> cancel{false};
  // st_dev -> pseudo-filesystem verdict (one statfs per device).
  std::mutex fs_mtx;
  std::unordered_map<uint64_t, bool> fs_skip;
};

bool pop_dir(ScanShared& sh, std::string& out) {
  std::lock_guard<std::mutex> lk(sh.mtx);
  if (sh.dir_stack.empty()) return false;
  out = std::move(sh.dir_stack.back());
  sh.dir_stack.pop_back();
  return true;
}

void push_dir(ScanShared& sh, std::string dir) {
  std::lock_guard<std::mutex> lk(sh.mtx);
  sh.dir_stack.push_back(std::move(dir));
  sh.pending.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

// Standard virtual locations: sizes there are kernel fiction, and walking
// them is slow, noisy, and error-prone. Boundary-aware (/proc2 must not
// match). /dev rides tmpfs magic on this box, so magic alone can't see it.
bool du_prune_dir_path(const std::string& full) {
  static constexpr const char* kVirt[] = {"/proc", "/sys", "/dev"};
  for (const char* v : kVirt) {
    size_t n = std::strlen(v);
    if (full.size() >= n && full.compare(0, n, v) == 0 &&
        (full.size() == n || full[n] == '/'))
      return true;
  }
  return false;
}

// Kernel-API filesystem families by statfs magic, wherever mounted.
// tmpfs is deliberately NOT here: /tmp and /run are real (if RAM-backed)
// space the user may want to see.
bool du_is_pseudo_fs(const std::string& path) {
  struct statfs sfs {};
  if (statfs(path.c_str(), &sfs) != 0) return false;
  switch (static_cast<uint32_t>(sfs.f_type)) {
    case 0x9fa0u:     // proc
    case 0x62656572u: // sysfs
    case 0x27e0ebu:   // cgroup v1
    case 0x63677270u: // cgroup v2
    case 0x64626720u: // debugfs
    case 0x74726163u: // tracefs
    case 0x73636673u: // securityfs
    case 0x62656570u: // configfs
    case 0xf97cff8cu: // selinuxfs
    case 0xcafe4e11u: // bpf
    case 0x6e736673u: // nsfs
    case 0x1cd1u:     // devpts
      return true;
    default:
      return false;
  }
}

// Combined verdict with a per-device verdict cache (one statfs per device;
// the common path is a hash hit per directory).
bool du_prune_dir(ScanShared& sh, const std::string& full, uint64_t dev) {
  if (du_prune_dir_path(full)) return true;
  {
    std::lock_guard<std::mutex> lk(sh.fs_mtx);
    auto it = sh.fs_skip.find(dev);
    if (it != sh.fs_skip.end()) return it->second;
  }
  bool skip = du_is_pseudo_fs(full);
  {
    std::lock_guard<std::mutex> lk(sh.fs_mtx);
    sh.fs_skip[dev] = skip;
  }
  return skip;
}

std::string du_file_ext(const std::string& name) {
  auto dot = name.rfind('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return {};
  std::string ext = name.substr(dot + 1);
  for (auto& c : ext)
    c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// Out-of-process enumeration via horizon-du-scan(1). Fills out_files/out_dirs
// and sh.errors/sh.pruned, driving du.p_* progress + ~2Hz redraws while the
// helper streams. Returns true when the helper ran (results valid unless
// du.cancel got set); false when disabled/missing/failed so the caller falls
// back to the in-process parallel walker.
static bool du_enumerate_via_helper(AppState* ap, const std::string& root,
                                    std::vector<RawFile>& out_files,
                                    std::unordered_set<std::string>& out_dirs,
                                    ScanShared& sh) {
  if (const char* e = std::getenv("EH_DU_HELPER"))
    if (*e && e[0] == '0') return false;
  auto& du = ap->diskusage;

  int outfds[2] = {-1, -1}, errfds[2] = {-1, -1};
  if (::pipe(outfds) != 0) return false;
  if (::pipe(errfds) != 0) {
    ::close(outfds[0]);
    ::close(outfds[1]);
    return false;
  }
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(outfds[0]);
    ::close(outfds[1]);
    ::close(errfds[0]);
    ::close(errfds[1]);
    return false;
  }
  if (pid == 0) {
    ::dup2(outfds[1], STDOUT_FILENO);
    ::dup2(errfds[1], STDERR_FILENO);
    ::close(outfds[0]);
    ::close(outfds[1]);
    ::close(errfds[0]);
    ::close(errfds[1]);
    if (const char* bin = std::getenv("EH_DU_HELPER_BIN"))
      ::execl(bin, "horizon-du-scan", root.c_str(), (char*)nullptr);
    ::execlp("horizon-du-scan", "horizon-du-scan", root.c_str(),
             (char*)nullptr);
    _exit(127);
  }
  ::close(outfds[1]);
  ::close(errfds[1]);
  int fl = ::fcntl(outfds[0], F_GETFL, 0);
  if (fl >= 0) ::fcntl(outfds[0], F_SETFL, fl | O_NONBLOCK);

  std::string buf;
  buf.reserve(65536);
  char tmp[32768];
  size_t n_records = 0;
  auto last_post = std::chrono::steady_clock::now();
  bool cancelled = false;

  auto post_progress = [&] {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double, std::milli>(now - last_post).count() <
        500.0)
      return;
    last_post = now;
    DeferredCall::callLater([ap]() {
      if (ap->du_open) {
        ap->du_pendingRedraw = true;
        draw_diskusage_window(*ap);
      }
    });
  };

  auto handle_record = [&](const char* beg, const char* end) {
    // "<size>\t<dev>\t<ino>\t<kind>\t<path>" — split the 4 tabs from the
    // left so paths containing tabs survive.
    const char* tabs[4] = {};
    const char* p = beg;
    for (int i = 0; i < 4; ++i) {
      p = static_cast<const char*>(std::memchr(p, '\t', end - p));
      if (!p) return;
      tabs[i] = p++;
    }
    uint64_t size = std::strtoull(beg, nullptr, 10);
    uint64_t dev = std::strtoull(tabs[0] + 1, nullptr, 10);
    uint64_t ino = std::strtoull(tabs[1] + 1, nullptr, 10);
    char kind = tabs[2][1];
    std::string path(tabs[3] + 1, end);
    if (kind == 'F') {
      RawFile f;
      f.path = std::move(path);
      f.size = size;
      f.dev = dev;
      f.ino = ino;
      du.p_files.fetch_add(1, std::memory_order_relaxed);
      du.p_bytes.fetch_add(size, std::memory_order_relaxed);
      out_files.push_back(std::move(f));
    } else if (kind == 'D') {
      out_dirs.insert(std::move(path));
      du.p_dirs.fetch_add(1, std::memory_order_relaxed);
    }
    ++n_records;
  };

  for (;;) {
    if (du.cancel.load(std::memory_order_relaxed)) {
      cancelled = true;
      break;
    }
    struct pollfd pfd{};
    pfd.fd = outfds[0];
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 50);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) {
      post_progress();
      continue;
    }
    if (pfd.revents & (POLLIN | POLLHUP)) {
      ssize_t n = ::read(outfds[0], tmp, sizeof(tmp));
      if (n > 0) {
        buf.append(tmp, static_cast<size_t>(n));
        size_t pos = 0;
        for (;;) {
          size_t z = buf.find('\0', pos);
          if (z == std::string::npos) break;
          handle_record(buf.data() + pos, buf.data() + z);
          pos = z + 1;
        }
        buf.erase(0, pos);
        post_progress();
      } else if (n == 0) {
        break;
      } else if (errno != EAGAIN && errno != EINTR) {
        break;
      }
    }
    if (pfd.revents & (POLLERR | POLLNVAL)) break;
  }
  ::close(outfds[0]);

  // Footer carries the helper's error/prune counters.
  std::string errbuf;
  char ebuf[1024];
  for (;;) {
    ssize_t n = ::read(errfds[0], ebuf, sizeof(ebuf));
    if (n > 0)
      errbuf.append(ebuf, static_cast<size_t>(n));
    else if (n == 0)
      break;
    else if (errno != EAGAIN && errno != EINTR)
      break;
    else if (n < 0 && errno == EAGAIN) {
      struct pollfd pfd{};
      pfd.fd = errfds[0];
      pfd.events = POLLIN;
      if (::poll(&pfd, 1, 200) <= 0) break;
    }
  }
  ::close(errfds[0]);
  uint64_t h_errors = 0, h_pruned = 0;
  if (const char* s = std::strstr(errbuf.c_str(), "DU-STATS")) {
    unsigned long long he = 0, hp = 0;
    if (std::sscanf(s, "DU-STATS errors=%llu pruned=%llu", &he, &hp) == 2) {
      h_errors = static_cast<uint64_t>(he);
      h_pruned = static_cast<uint64_t>(hp);
    }
  }
  sh.errors.fetch_add(h_errors);
  sh.pruned.fetch_add(h_pruned);
  du.p_errors.fetch_add(h_errors);

  if (cancelled) ::kill(pid, SIGKILL);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (!cancelled && n_records == 0 && WIFEXITED(status) &&
      WEXITSTATUS(status) == 127)
    return false;  // helper missing: fall back in-process
  return true;
}

void du_start_scan(AppState& app) {  auto& du = app.diskusage;
  du_stop_scan(app);
  // New data coming: the duplicate index reads these vectors, so stop
  // its workers first, then clear (stale groups would point nowhere).
  du_stop_dup_scan(app);
  du_stop_dup_trash(app);
  du.dup_groups.clear();
  du.dup_rows.clear();
  du.dup_done = false;
  du.dup_confirm_armed = false;
  du.files.clear();
  du.dirs.clear();
  du.exts.clear();
  du.tiles.clear();
  du.du_index.clear();
  du.du_data_gen++; // cached index holds views into the cleared vectors
  du.done = false;
  du.total_files = du.total_dirs = du.total_bytes = du.unknown_dirs = 0;
  du.pruned_dirs = 0;
  du.cur_dir = du.root;
  du.treemap_root = du.root;
  du.selected.clear();
  du.highlight_ext.clear();
  du.scroll_dir = du.scroll_ext = 0;
  du.cancel.store(false);
  du.scanning.store(true);
  du.p_files.store(0);
  du.p_dirs.store(0);
  du.p_bytes.store(0);
  du.p_errors.store(0);

  AppState* ap = &app;
  du.scan_thread = std::thread([ap]() {    auto& du = ap->diskusage;
    const std::string root = du.root;
    ScanShared sh;
    sh.dir_stack.push_back(root);
    sh.pending.store(1);
    struct Local {
      std::vector<RawFile> files;
      std::unordered_set<std::string> dirs;
    };
    std::mutex merge_mtx;
    std::vector<RawFile> all_files;
    std::unordered_set<std::string> all_dirs;
    std::thread pump;  // fallback progress pump; unused on the helper path

    // Prefer the out-of-process enumerator so the walk + record buffers
    // live outside horizon-files RSS; fall back to the parallel walker.
    bool helper_ok =
        du_enumerate_via_helper(ap, root, all_files, all_dirs, sh);
    if (helper_ok && du.cancel.load(std::memory_order_relaxed)) {
      du.scanning.store(false);
      return;
    }
    if (!helper_ok) {
    int nthreads = static_cast<int>(std::thread::hardware_concurrency());
    if (nthreads < 2) nthreads = 2;
    if (nthreads > 16) nthreads = 16;
    std::vector<std::thread> workers;
    for (int w = 0; w < nthreads; ++w) {
      workers.emplace_back([&, w] {
        Local local;
        local.files.reserve(8192);
        std::string dir;
        while (!du.cancel.load(std::memory_order_relaxed)) {
          if (!pop_dir(sh, dir)) {
            if (sh.pending.load(std::memory_order_acquire) == 0) break;
            std::this_thread::yield();
            continue;
          }
          DIR* d = opendir(dir.c_str());
          if (!d) {
            sh.errors.fetch_add(1, std::memory_order_relaxed);
            du.p_errors.fetch_add(1, std::memory_order_relaxed);
            if (sh.pending.fetch_sub(1) == 1) break;
            continue;
          }
          struct dirent* de;
          while ((de = readdir(d)) != nullptr) {
            if (du.cancel.load(std::memory_order_relaxed)) break;
            const char* name = de->d_name;
            if (name[0] == '.' &&
                (name[1] == '\0' ||
                 (name[1] == '.' && name[2] == '\0')))
              continue;
            std::string full = dir;
            if (!full.empty() && full.back() != '/') full += '/';
            full += name;
            struct stat st {};
            if (lstat(full.c_str(), &st) != 0) {
              sh.errors.fetch_add(1, std::memory_order_relaxed);
              du.p_errors.fetch_add(1, std::memory_order_relaxed);
              continue;
            }
            if (S_ISLNK(st.st_mode)) continue; // never follow symlinks
            if (S_ISDIR(st.st_mode)) {
              // Virtual filesystems (/proc, /sys, /dev, cgroups…): sizes
              // are kernel fiction. Prune, don't count, don't descend.
              if (du_prune_dir(sh, full,
                               static_cast<uint64_t>(st.st_dev))) {
                sh.pruned.fetch_add(1, std::memory_order_relaxed);
                continue;
              }
              local.dirs.insert(full);
              push_dir(sh, full);
              sh.dirs.fetch_add(1, std::memory_order_relaxed);
              du.p_dirs.fetch_add(1, std::memory_order_relaxed);
            } else if (S_ISREG(st.st_mode)) {
              RawFile f;
              f.path = std::move(full);
              f.size = static_cast<uint64_t>(st.st_size);
              f.dev = static_cast<uint64_t>(st.st_dev);
              f.ino = static_cast<uint64_t>(st.st_ino);
              local.files.push_back(std::move(f));
              sh.files.fetch_add(1, std::memory_order_relaxed);
              du.p_files.fetch_add(1, std::memory_order_relaxed);
              du.p_bytes.fetch_add(f.size, std::memory_order_relaxed);
            }
          }
          closedir(d);
          if (sh.pending.fetch_sub(1) == 1) break;
        }
        if (!local.files.empty() || !local.dirs.empty()) {
          std::lock_guard<std::mutex> lk(merge_mtx);
          all_files.insert(all_files.end(),
                           std::make_move_iterator(local.files.begin()),
                           std::make_move_iterator(local.files.end()));
          all_dirs.merge(local.dirs);
        }
      });
    }
    // Progress pump (~2Hz) while workers run.
    pump = std::thread([ap]() {
      auto& du = ap->diskusage;
      while (du.scanning.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!du.scanning.load(std::memory_order_relaxed)) break;
        DeferredCall::callLater([ap]() {
          if (ap->du_open) {
            ap->du_pendingRedraw = true;
            draw_diskusage_window(*ap);
          }
        });
      }
    });
    for (auto& t : workers) t.join();
    if (du.cancel.load()) {
      // Stopped: workers are joined, vectors untouched. Drop the flag so
      // the progress pump exits, then join it.
      du.scanning.store(false);
      pump.join();
      return;
    }
    }  // end in-process fallback walker
    // Merge (scanning stays true throughout): the UI thread keeps painting
    // the previous results and never lays out mid-merge, so the worker's
    // writes can't race paint. The flag drops only below, after merge.
    // Merge: sort files by path, dedupe hardlinks, bottom-up dir tree.
    std::sort(all_files.begin(), all_files.end(),
              [](const RawFile& a, const RawFile& b) {
                return a.path < b.path;
              });
    du.files.reserve(all_files.size());
    std::unordered_set<DevIno, DevInoHash> seen_links;
    seen_links.reserve(all_files.size() / 8 + 1024);
    std::unordered_map<std::string, DiskDir> dirs;
    dirs.reserve(all_dirs.size() * 2 + 1024);
    struct ExtSum {
      uint64_t bytes = 0;
      uint64_t files = 0;
    };
    std::unordered_map<std::string, ExtSum> extmap;
    // Ensure every walked dir (even empty ones) has an entry.
    for (auto& d : all_dirs) dirs[d];
    dirs[root];
    for (auto& f : all_files) {
      DevIno key{f.dev, f.ino};
      uint64_t counted = f.size;
      if (!seen_links.insert(key).second) counted = 0; // hardlink: no double count
      auto slash = f.path.rfind('/');
      std::string name =
          (slash == std::string::npos) ? f.path : f.path.substr(slash + 1);
      DiskFile df;
      df.path = f.path;
      df.size = counted;
      df.ext = du_file_ext(name);
      auto& es = extmap[df.ext];
      es.bytes += counted;
      es.files += 1;
      du.files.push_back(std::move(df));
      // Add to every ancestor up to root.
      std::string dir =
          (slash == std::string::npos) ? root : f.path.substr(0, slash);
      for (;;) {
        auto& agg = dirs[dir];
        agg.bytes += counted;
        agg.files += 1;
        if (dir.size() <= root.size()) break;
        auto s2 = dir.rfind('/');
        if (s2 == std::string::npos || s2 == 0) {
          dir = root;
          continue;
        }
        dir.resize(s2);
        if (dir.size() < root.size()) dir = root;
      }
    }
    // Immediate-subdir counts for "N items" display.
    for (auto& [dpath, agg] : dirs) {
      if (dpath.size() <= root.size()) continue;
      std::string parent = dpath.substr(0, dpath.rfind('/'));
      if (parent.size() < root.size()) parent = root;
      auto it = dirs.find(parent);
      if (it != dirs.end() && parent != dpath) it->second.dirs += 1;
    }
    du.dirs = std::move(dirs);
    du.total_files = all_files.size();
    du.total_dirs = all_dirs.size();
    uint64_t total = 0;
    for (auto& f : du.files) total += f.size;
    du.total_bytes = total;
    du.unknown_dirs = sh.errors.load();
    du.pruned_dirs = sh.pruned.load();
    // Extension ranking + palette. EVERY ranked extension gets its own
    // color from a golden-ratio hue wheel: adjacent ranks land maximally
    // apart, so no two entries in the same view share a hue. Gray is
    // reserved for genuinely unmapped content (the "<smaller>" tail
    // bucket, unsubdivided leaf dirs) — never for a known extension.
    // Legend dots and treemap files share du.exts as the single source.
    auto rank_rgb = [](size_t i) {
      double hue = std::fmod(static_cast<double>(i) * 0.61803398875, 1.0);
      const double s = 0.62, v = 0.88;
      const double c = v * s;
      const double hh = hue * 6.0;
      const double x = c * (1.0 - std::fabs(std::fmod(hh, 2.0) - 1.0));
      double r = 0, g = 0, b = 0;
      int sector = static_cast<int>(hh) % 6;
      if (sector == 0) { r = c; g = x; }
      else if (sector == 1) { r = x; g = c; }
      else if (sector == 2) { g = c; b = x; }
      else if (sector == 3) { g = x; b = c; }
      else if (sector == 4) { r = x; b = c; }
      else { r = c; b = x; }
      const double m = v - c;
      return std::array<double, 3>{r + m, g + m, b + m};
    };
    std::vector<std::pair<std::string, ExtSum>> ranked(extmap.begin(),
                                                       extmap.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) {
                return a.second.bytes > b.second.bytes;
              });
    du.exts.clear();
    for (size_t i = 0; i < ranked.size(); ++i) {
      DiskExt e;
      e.ext = ranked[i].first;
      e.bytes = ranked[i].second.bytes;
      e.files = ranked[i].second.files;
      auto rgb = rank_rgb(i);
      e.r = rgb[0];
      e.g = rgb[1];
      e.b = rgb[2];
      du.exts.push_back(std::move(e));
    }
    du.done = true;
    // Merge complete: drop the flag (progress pump exits), join it, then
    // hand fresh vectors to the UI thread. The helper path posted progress
    // directly and started no pump.
    du.scanning.store(false);
    if (pump.joinable()) pump.join();
    DeferredCall::callLater([ap]() {
      // Publish on the UI thread: new data generation (invalidates the
      // cached treemap index), visible children for the current drill
      // level, then treemap refresh + redraw. A live duplicates view
      // rehashes over the fresh model.
      ap->diskusage.du_data_gen++;
      du_rebuild_kids(*ap);
      du_refresh_views(*ap);
      if (ap->diskusage.dup_mode) du_start_dup_scan(*ap);
      if (ap->du_open) {
        ap->du_pendingRedraw = true;
        draw_diskusage_window(*ap);
      }
    });
  });
}

void du_stop_scan(AppState& app) {
  auto& du = app.diskusage;
  du.cancel.store(true);
  du.scanning.store(false);
  // Workers poll cancel on every directory; the join below returns
  // promptly. Joinable (never detached) so rescan/close can't strand a
  // writer that would race the next scan's cleared vectors.
  if (du.scan_thread.joinable()) du.scan_thread.join();
}

} // namespace eh::file_browser
