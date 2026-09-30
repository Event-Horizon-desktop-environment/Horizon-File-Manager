// search.cpp — parallel walker + search workers (see grep.hpp).

#include "grep.hpp"
#include "ignore.hpp"
#include "matcher.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace hgrep {
namespace {

// Bounded path queue (walker -> workers).
class PathQueue {
 public:
  explicit PathQueue(size_t cap) : cap_(cap) {}
  void push(std::string p) {
    std::unique_lock<std::mutex> lk(m_);
    cv_space_.wait(lk, [&] { return queue_.size() < cap_ || done_; });
    if (done_) return;
    queue_.push_back(std::move(p));
    cv_item_.notify_one();
  }
  bool pop(std::string& out) {
    std::unique_lock<std::mutex> lk(m_);
    cv_item_.wait(lk, [&] { return !queue_.empty() || done_; });
    if (queue_.empty()) return false;
    out = std::move(queue_.back());
    queue_.pop_back();
    cv_space_.notify_one();
    return true;
  }
  // Drain up to max paths with a single wakeup (amortizes condvar
  // latency over small files). Blocks for the first like pop().
  size_t pop_batch(std::vector<std::string>& out, size_t max) {
    std::unique_lock<std::mutex> lk(m_);
    cv_item_.wait(lk, [&] { return !queue_.empty() || done_; });
    size_t n = 0;
    while (!queue_.empty() && n < max) {
      out.push_back(std::move(queue_.back()));
      queue_.pop_back();
      ++n;
    }
    if (n) cv_space_.notify_all();
    return n;
  }
  void finish() {
    std::lock_guard<std::mutex> lk(m_);
    done_ = true;
    cv_item_.notify_all();
    cv_space_.notify_all();
  }

 private:
  size_t cap_;
  std::mutex m_;
  std::condition_variable cv_item_, cv_space_;
  std::vector<std::string> queue_;
  bool done_ = false;
};

// Owned file contents: mmap for large files (zero-copy), streamed read
// for small ones (fewer syscalls than open+fstat+mmap+munmap+close).
struct FileData {
  const char* data = nullptr;
  size_t len = 0;
  std::string owned;
  void* mapping = nullptr;
  size_t map_len = 0;
  int map_fd = -1;
  void release() {
    if (mapping) {
      munmap(mapping, map_len);
      mapping = nullptr;
    }
    if (map_fd >= 0) {
      close(map_fd);
      map_fd = -1;
    }
    owned.clear();
    // No shrink_to_fit: workers reuse one FileData per thread so the
    // read buffer capacity survives the whole run (no malloc churn).
    data = nullptr;
    len = 0;
  }
};

// fd must be freshly opened O_RDONLY (ownership taken, closed on return).
// Small files stream; large files mmap (needs size via fstat).
bool load_file_fd(int fd, uint64_t max_size, FileData& out, bool& too_big) {
  static constexpr uint64_t kMmapMin = 256u * 1024u;
  static constexpr size_t kFirst = 131072;
  too_big = false;
  if (fd < 0) return false;
  out.owned.clear(); // keep capacity across files
  // Read-first: small files finish here with open+read+close only
  // (no fstat). Large files fall back to fstat+mmap below.
  char first[kFirst];
  ssize_t r0 = read(fd, first, sizeof(first));
  if (r0 < 0) {
    close(fd);
    out.release();
    return false;
  }
  if (r0 == 0) {
    close(fd); // empty file
    out.data = out.owned.data();
    out.len = 0;
    return true;
  }
  if (static_cast<size_t>(r0) < sizeof(first) &&
      static_cast<uint64_t>(r0) <= max_size) {
    out.owned.assign(first, static_cast<size_t>(r0));
    close(fd);
    out.data = out.owned.data();
    out.len = out.owned.size();
    return true;
  }
  struct stat st {};
  bool sized = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
  uint64_t size = sized ? static_cast<uint64_t>(st.st_size) : 0;
  if (sized && size >= kMmapMin) {
    if (size > max_size) {
      too_big = true;
      close(fd);
      return false;
    }
    void* m = mmap(nullptr, static_cast<size_t>(size), PROT_READ,
                   MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
      close(fd);
      return false;
    }
    madvise(m, static_cast<size_t>(size),
            MADV_SEQUENTIAL | MADV_WILLNEED);
    out.data = static_cast<const char*>(m);
    out.len = static_cast<size_t>(size);
    out.mapping = m;
    out.map_len = out.len;
    out.map_fd = fd;
    return true;
  }
  // Medium file (or unfstatable): stream from where the first read
  // left off. Contiguity is preserved for cross-chunk matches.
  if (static_cast<uint64_t>(r0) > max_size) {
    too_big = true;
    close(fd);
    out.release();
    return false;
  }
  out.owned.append(first, static_cast<size_t>(r0));
  uint64_t total = static_cast<uint64_t>(r0);
  char buf[65536];
  for (;;) {
    if (total > max_size) {
      too_big = true;
      close(fd);
      out.release();
      return false;
    }
    ssize_t r = read(fd, buf, sizeof(buf));
    if (r < 0) {
      close(fd);
      out.release();
      return false;
    }
    if (r == 0) break;
    out.owned.append(buf, static_cast<size_t>(r));
    total += static_cast<uint64_t>(r);
  }
  close(fd);
  out.data = out.owned.data();
  out.len = out.owned.size();
  return true;
}

bool has_nul_prefix(const char* data, size_t len) {
  size_t probe = len < 8192 ? len : 8192;
  return memchr(data, '\0', probe) != nullptr;
}

struct Shared {
  const GrepOptions* opt = nullptr;  std::function<bool(GrepMatch)> on_match;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> files_searched{0};
  std::atomic<uint64_t> files_skipped{0};
  std::atomic<uint64_t> bytes_searched{0};
  std::atomic<uint64_t> matches{0};
  std::mutex emit_mtx; // serializes on_match (CLI prints from workers)
};

// Search one file's bytes; emits matches. Returns false to stop everything.
bool search_bytes(const std::string& path, const char* data, size_t len,
                  const GrepOptions& opt, const detail::HorspoolCI* hs,
                  Shared& shared, const std::atomic<bool>& cancel) {
  if (cancel.load(std::memory_order_relaxed) ||
      shared.stop.load(std::memory_order_relaxed))
    return false;
  // Binary files: report the first hit as a marker (rg parity), without
  // line extraction — the content isn't text.
  if (len > 0 && has_nul_prefix(data, len)) {
    size_t nlen = opt.pattern.size();
    size_t first = static_cast<size_t>(-1);
    auto grab = [&](size_t off) -> bool {
      first = off;
      return false;
    };
    if (opt.case_insensitive)
      find_all_ci(data, len, *hs, grab);
    else
      find_all_exact(data, len, opt.pattern.data(), nlen, grab);
    if (first != static_cast<size_t>(-1)) {
      GrepMatch m;
      m.path = path;
      m.line_no = 0;
      m.byte_offset = first;
      {
        std::lock_guard<std::mutex> lk(shared.emit_mtx);
        if (!shared.on_match(std::move(m))) {
          shared.stop.store(true);
          return false;
        }
      }
      shared.matches.fetch_add(1, std::memory_order_relaxed);
    } else {
      shared.files_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  }
  size_t nlen = opt.pattern.size();
  uint64_t line_no = 1;
  size_t line_start = 0;
  enum class HitAction { Continue, StopFile, StopAll };
  bool stop_all = false;
  auto on_off = [&](size_t off) -> HitAction {
    // Advance line tracking to the match.
    const char* base = data + line_start;
    size_t span = off >= line_start ? off - line_start : 0;
    const char* p = base;
    const char* end = base + span;
    while (p < end) {
      const void* nl = memchr(p, '\n', static_cast<size_t>(end - p));
      if (!nl) break;
      ++line_no;
      p = static_cast<const char*>(nl) + 1;
    }
    line_start = static_cast<size_t>(p - data);
    if (opt.files_only) {
      GrepMatch m;
      m.path = path;
      m.byte_offset = off;
      {
        std::lock_guard<std::mutex> lk(shared.emit_mtx);
        if (!shared.on_match(std::move(m))) return HitAction::StopAll;
      }
      shared.matches.fetch_add(1, std::memory_order_relaxed);
      return HitAction::StopFile; // one hit per file is enough
    }
    // Extract the line (capped).
    size_t lend = off;
    const void* nl =
        memchr(data + off, '\n', len > off ? len - off : 0);
    lend = nl ? static_cast<size_t>(static_cast<const char*>(nl) - data)
              : len;
    size_t lstart = line_start;
    size_t llen = lend > lstart ? lend - lstart : 0;
    if (llen > 0 && data[lend - 1] == '\r') --llen;
    static constexpr size_t kLineCap = 4096;
    size_t take = llen < kLineCap ? llen : kLineCap;
    GrepMatch m;
    m.path = path;
    m.line_no = line_no;
    m.byte_offset = off;
    m.line.assign(data + lstart, take);
    {
      std::lock_guard<std::mutex> lk(shared.emit_mtx);
      if (!shared.on_match(std::move(m))) return HitAction::StopAll;
    }
    shared.matches.fetch_add(1, std::memory_order_relaxed);
    return HitAction::Continue;
  };
  bool file_done = false;
  auto pump = [&](size_t off) -> bool {
    if (file_done) return false;
    HitAction a = on_off(off);
    if (a == HitAction::StopFile) {
      file_done = true;
      return false;
    }
    if (a == HitAction::StopAll) {
      stop_all = true;
      return false;
    }
    return true;
  };
  if (opt.case_insensitive)
    find_all_ci(data, len, *hs, pump);
  else
    find_all_exact(data, len, opt.pattern.data(), nlen, pump);
  if (stop_all) {
    shared.stop.store(true);
    return false;
  }
  return true;
}

// count_only needs per-file totals, not per-hit emits. This wrapper
// recounts: run the scan counting hits, then emit one summary match.
bool search_file_count(const std::string& path, const char* data, size_t len,
                       const GrepOptions& opt, const detail::HorspoolCI* hs,
                       Shared& shared, const std::atomic<bool>& cancel) {
  if (cancel.load(std::memory_order_relaxed) ||
      shared.stop.load(std::memory_order_relaxed))
    return false;
  if (len > 0 && has_nul_prefix(data, len)) {
    // Same binary-marker rule as search_bytes (counts as one line).
    size_t nlen = opt.pattern.size();
    bool hit = false;
    auto grab = [&](size_t) -> bool {
      hit = true;
      return false;
    };
    if (opt.case_insensitive)
      find_all_ci(data, len, *hs, grab);
    else
      find_all_exact(data, len, opt.pattern.data(), nlen, grab);
    if (!hit) {
      shared.files_skipped.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    // Binary hit: counts as one line, skip the text scan below.
    GrepMatch m;
    m.path = path;
    m.line_no = 1;
    m.byte_offset = 0;
    bool cont = true;
    {
      std::lock_guard<std::mutex> lk(shared.emit_mtx);
      cont = shared.on_match(std::move(m));
    }
    shared.matches.fetch_add(1, std::memory_order_relaxed);
    if (!cont) {
      shared.stop.store(true);
      return false;
    }
    return true;
  }
  size_t nlines = 0;
  uint64_t line_no = 1;
  size_t line_start = 0;
  size_t last_counted_start = static_cast<size_t>(-1);
  auto counter = [&](size_t off) -> bool {
    // Advance to the hit's line; count each line once (rg parity).
    const char* base = data + line_start;
    size_t span = off >= line_start ? off - line_start : 0;
    const char* p = base;
    const char* end = base + span;
    while (p < end) {
      const void* nl = memchr(p, '\n', static_cast<size_t>(end - p));
      if (!nl) break;
      ++line_no;
      p = static_cast<const char*>(nl) + 1;
    }
    line_start = static_cast<size_t>(p - data);
    if (line_start != last_counted_start) {
      last_counted_start = line_start;
      ++nlines;
    }
    return true;
  };
  if (opt.case_insensitive)
    find_all_ci(data, len, *hs, counter);
  else
    find_all_exact(data, len, opt.pattern.data(), opt.pattern.size(),
                   counter);
  if (nlines > 0) {
    GrepMatch m;
    m.path = path;
    m.line_no = nlines; // count rides here for -c
    m.byte_offset = 0;
    bool cont = true;
    {
      std::lock_guard<std::mutex> lk(shared.emit_mtx);
      cont = shared.on_match(std::move(m));
    }
    shared.matches.fetch_add(nlines, std::memory_order_relaxed);
    if (!cont) {
      shared.stop.store(true);
      return false;
    }
  }
  return true;
}


void worker_fn(PathQueue& queue, const GrepOptions& opt,
               const detail::HorspoolCI* hs, Shared& shared,
               const std::atomic<bool>& cancel) {
  std::vector<std::string> batch;
  batch.reserve(64);
  FileData fdata; // reused across files: capacity survives the run
  for (;;) {
    batch.clear();
    if (queue.pop_batch(batch, 64) == 0) return;
    for (auto& path : batch) {
      if (cancel.load(std::memory_order_relaxed) ||
          shared.stop.load(std::memory_order_relaxed))
        continue;
    // No fstat: regular files ignore O_NONBLOCK, so a non-blocking
    // first read inside load either yields bytes or EAGAIN (fifo /
    // socket / TOCTOU race), which load reports as unreadable.
    int fd = open(path.c_str(),
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
      // Symlinked file (O_NOFOLLOW refuses): fall back to stat().
      struct stat st {};
      if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        shared.files_skipped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
      if (fd < 0) {
        shared.files_skipped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
    }
    FileData fdata;
    bool too_big = false;
    if (!load_file_fd(fd, opt.max_filesize, fdata, too_big)) {
      // Unreadable, or larger than the cap.
      if (too_big)
        shared.files_skipped.fetch_add(1, std::memory_order_relaxed);
      else
        shared.files_skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (fdata.len == 0) {
      // Empty files can't match a non-empty pattern.
      shared.files_searched.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    shared.files_searched.fetch_add(1, std::memory_order_relaxed);
    shared.bytes_searched.fetch_add(fdata.len, std::memory_order_relaxed);
    bool cont;
    if (opt.count_only)
      cont = search_file_count(path, fdata.data, fdata.len, opt, hs, shared,
                               cancel);
    else
      cont = search_bytes(path, fdata.data, fdata.len, opt, hs, shared,
                          cancel);
    fdata.release();
    if (!cont) shared.stop.store(true);
    }
  }
}

struct WalkEntry {
  std::string abs_path;
  std::string rel; // root-relative, "" for the root itself
};


inline bool is_hidden_name(const char* name) { return name[0] == '.'; }

struct Frame {
  WalkEntry e;
  DIR* dir = nullptr;
};

// Iterative core over an explicit frame stack. The caller's ignore levels
// for everything ABOVE the seeds must already be pushed (each thread owns
// its stack); levels pushed here are popped before returning.
void walk_frames(std::vector<Frame>& frames, const GrepOptions& opt,
                 PathQueue& queue, IgnoreStack& ignores,
                 const std::atomic<bool>& cancel) {
  while (!frames.empty()) {
    if (cancel.load(std::memory_order_relaxed)) break;
    Frame& fr = frames.back();
    if (!fr.dir) {
      fr.dir = opendir(fr.e.abs_path.c_str());
      if (!fr.dir) {
        ignores.pop_dir();
        frames.pop_back();
        continue;
      }
    }
    errno = 0;
    struct dirent* de = readdir(fr.dir);
    if (!de) {
      closedir(fr.dir);
      ignores.pop_dir();
      frames.pop_back();
      continue;
    }
    const char* name = de->d_name;
    if (name[0] == '.' && (name[1] == '\0' ||
                           (name[1] == '.' && name[2] == '\0')))
      continue;
    if (!opt.hidden && is_hidden_name(name)) continue;
    if (strcmp(name, ".git") == 0) continue; // never index git internals
    std::string abs = fr.e.abs_path;
    if (!abs.empty() && abs.back() != '/') abs += '/';
    abs += name;
    std::string rel = fr.e.rel.empty() ? name : fr.e.rel + "/" + name;
    unsigned char type = de->d_type;
    struct stat st {};
    bool need_stat = (type == DT_UNKNOWN || type == DT_LNK);
    if (need_stat && lstat(abs.c_str(), &st) != 0) continue;
    bool is_link = (type == DT_LNK) || (need_stat && S_ISLNK(st.st_mode));
    bool is_dir =
        (type == DT_DIR) || (need_stat && S_ISDIR(st.st_mode));
    if (is_link && is_dir && !opt.follow_symlink_dirs) continue;
    if (is_dir) {
      if (!opt.no_ignore && ignores.ignored(rel, true)) continue;
      ignores.push_dir(abs, rel);
      frames.push_back({{abs, rel}});
    } else {
      // Regular files only: file symlinks are skipped in recursion
      // (rg parity — avoids reporting every match twice). An explicit
      // symlink root is still followed (see walk_root).
      bool is_file = (type == DT_REG) ||
                     (need_stat && S_ISREG(st.st_mode) && !is_link);
      if (!is_file) continue;
      if (!opt.no_ignore && ignores.ignored(rel, false)) continue;
      queue.push(abs);
    }
  }
}

// Root fan-out: the calling thread lists the root (files go straight to
// the queue); subdirectories are dealt round-robin to walker threads so
// readdir/lstat parallelizes. Each thread owns an IgnoreStack seeded
// with the root level.
void walk_root(const std::string& root, const GrepOptions& opt,
               PathQueue& queue, int nwalkers,
               const std::atomic<bool>& cancel) {
  struct stat rst {};
  if (lstat(root.c_str(), &rst) != 0) return;
  if (S_ISREG(rst.st_mode) || S_ISLNK(rst.st_mode)) {
    queue.push(root);
    return;
  }
  if (!S_ISDIR(rst.st_mode)) return;
  DIR* dir = opendir(root.c_str());
  if (!dir) return;
  IgnoreStack root_ignores;
  root_ignores.push_dir(root, "");
  std::vector<std::pair<std::string, std::string>> subdirs;
  struct dirent* de;
  while ((de = readdir(dir)) != nullptr) {
    const char* name = de->d_name;
    if (name[0] == '.' && (name[1] == '\0' ||
                           (name[1] == '.' && name[2] == '\0')))
      continue;
    if (!opt.hidden && is_hidden_name(name)) continue;
    if (strcmp(name, ".git") == 0) continue;
    std::string abs = root;
    if (!abs.empty() && abs.back() != '/') abs += '/';
    abs += name;
    std::string rel = name;
    unsigned char type = de->d_type;
    struct stat st {};
    bool need_stat = (type == DT_UNKNOWN || type == DT_LNK);
    if (need_stat && lstat(abs.c_str(), &st) != 0) continue;
    bool is_link = (type == DT_LNK) || (need_stat && S_ISLNK(st.st_mode));
    bool is_dir = (type == DT_DIR) || (need_stat && S_ISDIR(st.st_mode));
    if (is_link && is_dir && !opt.follow_symlink_dirs) continue;
    if (is_dir) {
      if (!opt.no_ignore && root_ignores.ignored(rel, true)) continue;
      subdirs.emplace_back(abs, rel);
    } else {
      // Same symlink rule as the recursive walker below (rg parity).
      bool is_file = (type == DT_REG) ||
                     (need_stat && S_ISREG(st.st_mode) && !is_link);
      if (!is_file) continue;
      if (!opt.no_ignore && root_ignores.ignored(rel, false)) continue;
      queue.push(abs);
    }
  }
  closedir(dir);
  int nwalk = std::min(std::max(nwalkers / 2, 1), 4);
  std::atomic<size_t> next{0};
  std::vector<std::thread> walkers;
  for (int w = 0; w < nwalk; ++w) {
    walkers.emplace_back([&, w] {
      IgnoreStack ignores;
      ignores.push_dir(root, "");
      for (;;) {
        size_t i = next.fetch_add(1);
        if (i >= subdirs.size() || cancel.load(std::memory_order_relaxed))
          break;
        ignores.push_dir(subdirs[i].first, subdirs[i].second);
        std::vector<Frame> frames;
        frames.push_back({{subdirs[i].first, subdirs[i].second}});
        walk_frames(frames, opt, queue, ignores, cancel);
      }
    });
  }
  for (auto& t : walkers) t.join();
}

} // namespace

// Default pool size: measured sweet spot (j=8 beats j=24 on a 24-core
// box — more threads just contend on the queue/emit locks).
int default_threads() {
  unsigned hw = std::thread::hardware_concurrency();
  if (hw <= 4) return static_cast<int>(hw > 0 ? hw : 4);
  int n = static_cast<int>(hw / 3);
  if (n < 4) n = 4;
  if (n > 8) n = 8;
  return n;
}

bool hgrep_search(const std::string& root, const GrepOptions& opt,
                  const std::function<bool(GrepMatch)>& on_match,
                  const std::atomic<bool>& cancel, GrepStats& stats) {
  stats = GrepStats{};
  if (opt.pattern.empty() || root.empty()) return true;
  int nthreads = opt.threads > 0 ? opt.threads : default_threads();
  if (nthreads < 1) nthreads = 1;
  if (nthreads > 64) nthreads = 64;
  Shared shared;
  shared.opt = &opt;
  shared.on_match = on_match;
  detail::HorspoolCI hs(opt.pattern);
  const detail::HorspoolCI* hsp = opt.case_insensitive ? &hs : nullptr;

  PathQueue queue(2048);
  std::thread walker([&] {
    walk_root(root, opt, queue, nthreads, cancel);
    queue.finish();
  });
  std::vector<std::thread> workers;
  for (int i = 0; i < nthreads; ++i)
    workers.emplace_back([&] {
      worker_fn(queue, opt, hsp, shared, cancel);
    });
  walker.join();
  for (auto& t : workers) t.join();
  stats.files_searched = shared.files_searched.load();
  stats.files_skipped = shared.files_skipped.load();
  stats.bytes_searched = shared.bytes_searched.load();
  stats.matches = shared.matches.load();
  return true;
}

} // namespace hgrep
