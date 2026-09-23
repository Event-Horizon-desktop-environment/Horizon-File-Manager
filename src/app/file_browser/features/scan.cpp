// scan.cpp
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
#include "app/file_browser/features/filetype.hpp"
#include "app/file_browser/features/nav.hpp"


namespace eh::file_browser {
static void recompute_item_counts(Tab& tab, bool show_hidden);
}
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
// Digit-aware "natural" comparison: embedded number runs compare by value,
// so file2 < file10. Honors case sensitivity preference.
static int natural_compare(const std::string& a, const std::string& b,
                           bool case_sensitive) {
  size_t i = 0, j = 0;
  auto fold = [&](unsigned char c) {
    return case_sensitive ? c : static_cast<unsigned char>(std::tolower(c));
  };
  while (i < a.size() && j < b.size()) {
    unsigned char ca = a[i], cb = b[j];
    if (std::isdigit(ca) && std::isdigit(cb)) {
      // Skip leading zeros but remember there was one
      size_t si = i, sj = j;
      while (si < a.size() && a[si] == '0') ++si;
      while (sj < b.size() && b[sj] == '0') ++sj;
      size_t ei = si, ej = sj;
      while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
      while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
      size_t li = ei - si, lj = ej - sj;
      if (li != lj) return li < lj ? -1 : 1;
      while (si < ei) {
        if (a[si] != b[sj]) return a[si] < b[sj] ? -1 : 1;
        ++si; ++sj;
      }
      i = ei; j = ej;
    } else {
      unsigned char fa = fold(ca), fb = fold(cb);
      if (fa != fb) return fa < fb ? -1 : 1;
      ++i; ++j;
    }
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return 0;
}
// ── Background directory scan ────────────────────────────────────────
//
// Canonical sort key whose byte-order exactly reproduces
// natural_compare(a, b, case_sensitive=false). Digit runs are zero-padded to
// a fixed width so run-length and numeric rules fall out of plain memcmp;
// a NUL + raw-name tail preserves ties/prefix quirks identically.
static void natural_key(const std::string& name, std::string& out) {
  out.clear();
  out.reserve(name.size() + 16);
  constexpr size_t W = 18;   // max digit-run width kept exact
  size_t i = 0;
  while (i < name.size()) {
    unsigned char c = static_cast<unsigned char>(name[i]);
    if (std::isdigit(c)) {
      size_t si = i;
      while (si < name.size() && name[si] == '0') ++si;
      size_t ei = si;
      while (ei < name.size() &&
             std::isdigit(static_cast<unsigned char>(name[ei]))) ++ei;
      size_t len = ei - si;                 // significant digits
      size_t pad = len < W ? W - len : 0;
      out.append(pad, '0');
      out.append(name, si, ei - si);
      i = ei;
    } else {
      out.push_back(static_cast<char>(std::tolower(c)));
      ++i;
    }
  }
}

struct EntryLess {
  const ScanParams* sp;
  // Snapshot once per sort: group_rank(Date) must not call time() per
  // comparison (O(n log n) syscalls). Set at every construction site.
  std::time_t now = 0;
  // Group By rank helper (field: 0=None 1=Type 2=Name 3=Date 4=Size)
  int group_rank(const FileEntry& e) const {
    switch (sp->group_field) {
      case 1: return static_cast<int>(e.type);
      case 2: {
        if (e.name.empty()) return 0;
        unsigned char c0 = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(e.name[0])));
        return (c0 >= 'a' && c0 <= 'z') ? c0 : ('z' + 1);
      }
      case 3: {
        std::time_t t = now ? now : std::time(nullptr);
        double age_s = difftime(t, e.modified_sec);
        if (age_s < 0) age_s = 0;
        if (age_s < 86400) return 0;                     // Today
        if (age_s < 172800) return 1;                    // Yesterday
        if (age_s < 7 * 86400) return 2;                 // This Week
        if (age_s < 30 * 86400) return 3;                // This Month
        if (age_s < 365 * 86400) return 4;               // This Year
        return 5;                                        // Earlier
      }
      case 4:
        if (e.is_dir) return 0;                          // Folders
        if (e.size < 100ull * 1024) return 1;            // Small
        if (e.size < 10ull * 1024 * 1024) return 2;      // Medium
        if (e.size < 1024ull * 1024 * 1024) return 3;    // Large
        return 4;                                        // Huge
      default: return 0;
    }
  }

  bool operator()(const FileEntry& a, const FileEntry& b) const {
    return less(a, b);
  }

  bool less(const FileEntry& a, const FileEntry& b) const {
      if (sp->folders_first && a.is_dir != b.is_dir) return a.is_dir;

      // Hidden entries optionally sort after everything else
      if (sp->hidden_last && a.is_hidden != b.is_hidden) return !a.is_hidden;

      // Group By: primary sort key when enabled
      if (sp->group_field > 0) {
        int ga = group_rank(a);
        int gb = group_rank(b);
        if (ga != gb) return ga < gb;
      }

      auto name_cmp = [&](const std::string& x, const std::string& y) -> int {
        if (sp->natural)
          return natural_compare(x, y, sp->case_sensitive);
        if (sp->case_sensitive)
          return x.compare(y);
        // Plain case-folded lexicographic
        size_t n = std::min(x.size(), y.size());
        for (size_t k = 0; k < n; ++k) {
          unsigned char cx = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(x[k])));
          unsigned char cy = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(y[k])));
          if (cx != cy) return cx < cy ? -1 : 1;
        }
        return x.size() < y.size() ? -1 : (x.size() > y.size() ? 1 : 0);
      };

      int cmp = 0;
      switch (static_cast<SortField>(sp->sort_field)) {
        case SortField::Name:
          cmp = name_cmp(a.name, b.name);
          break;
        case SortField::Size:
          if (a.size < b.size) cmp = -1;
          else if (a.size > b.size) cmp = 1;
          break;
        case SortField::Modified:
          if (a.modified_sec < b.modified_sec) cmp = -1;
          else if (a.modified_sec > b.modified_sec) cmp = 1;
          break;
        case SortField::FirstModified:
          if (a.modified_sec < b.modified_sec) cmp = -1;
          else if (a.modified_sec > b.modified_sec) cmp = 1;
          break;
        case SortField::LastModified:
          if (a.modified_sec > b.modified_sec) cmp = -1;
          else if (a.modified_sec < b.modified_sec) cmp = 1;
          break;
        case SortField::Type:
          cmp = static_cast<int>(a.type) - static_cast<int>(b.type);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
        case SortField::Owner:
          cmp = a.owner.compare(b.owner);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
        case SortField::Group:
          cmp = a.group.compare(b.group);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
        case SortField::Permissions:
          cmp = (a.mode & 0777) - (b.mode & 0777);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
        case SortField::Extension:
          cmp = a.extension.compare(b.extension);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
        case SortField::LinkTarget:
          cmp = a.link_target.compare(b.link_target);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
      }
      // FirstModified/LastModified have fixed directions, ignore descending
      if (static_cast<SortField>(sp->sort_field) == SortField::FirstModified)
        return cmp < 0;
      if (static_cast<SortField>(sp->sort_field) == SortField::LastModified)
        return cmp > 0;
      return sp->descending ? cmp > 0 : cmp < 0;
    }
};

static void sort_entries(std::vector<FileEntry>& entries, const ScanParams& sp) {
  std::sort(entries.begin(), entries.end(), EntryLess{&sp, std::time(nullptr)});
}

// Per-directory-entry raw data captured from readdir()/getdents64.
struct DirItem {
  std::string name;
  unsigned char dtype;
};

static const char* kXdgIcons[8] = {
    "user-desktop",   "folder-documents", "folder-download", "folder-music",
    "folder-pictures","folder-videos",    "folder-publicshare", "folder-templates"};
static const char* kXdgSubdirs[8] = {
    "/Desktop", "/Documents", "/Downloads", "/Music",
    "/Pictures", "/Videos", "/Public", "/Templates"};

// Metadata snapshot shared by the synchronous and io_uring code paths.
struct StatData {
  bool ok = false;
  mode_t mode = 0;
  uint64_t size = 0;
  int64_t mtime_sec = 0;
  uid_t uid = 0;
  gid_t gid = 0;
};

static StatData statdata_from_statx(int res, const struct statx& sx) {
  StatData d;
  if (res == 0 && (sx.stx_mask & STATX_BASIC_STATS)) {
    d.ok = true;
    d.mode = static_cast<mode_t>(sx.stx_mode);
    d.size = sx.stx_size;
    d.mtime_sec = static_cast<int64_t>(sx.stx_mtime.tv_sec);
    d.uid = sx.stx_uid;
    d.gid = sx.stx_gid;
  }
  return d;
}

// Build one FileEntry from a pre-fetched metadata snapshot
// (pure function of its inputs — safe on any thread).
static FileEntry make_entry_from(const DirItem& it, const StatData& sd,
                                 const std::string& prefix,
                                 const std::unordered_set<std::string>& hidden_names,
                                 const std::string xdg_full[8]) {
  std::string full;
  full.reserve(prefix.size() + it.name.size());
  full = prefix;
  full += it.name;

  const bool stat_ok = sd.ok;

  FileEntry e;
  e.name = it.name;
  e.path = full;
  e.in_hidden_file = hidden_names.count(it.name) > 0;
  e.is_hidden = is_hidden_file(it.name) || e.in_hidden_file;
  e.is_symlink = (it.dtype == DT_LNK);

  if (it.dtype == DT_DIR) {
    e.is_dir = true;
  } else if (stat_ok) {
    e.is_dir = S_ISDIR(sd.mode);
  }

  if (stat_ok) {
    e.size = sd.size;
    e.modified_sec = sd.mtime_sec;
    e.mode = sd.mode;
    e.readable =
        (sd.mode & (S_IRUSR | S_IRGRP | S_IROTH)) != 0 || geteuid() == 0;
    e.writable =
        (sd.mode & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0 || geteuid() == 0;
    e.owner = user_name(sd.uid);
    e.group = group_name(sd.gid);
    e.owned_by_root = (sd.uid == 0);
  }

  // Symlink target (for LinkTarget sorting / emblems)
  if (e.is_symlink) {
    char buf[4096];
    ssize_t n = readlink(full.c_str(), buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      e.link_target = buf;
    }
  }

  // Lowercase extension without the dot ("tar.gz" keeps full suffix chain)
  if (!e.is_dir) {
    auto dot = it.name.rfind('.');
    if (dot != std::string::npos && dot + 1 < it.name.size()) {
      e.extension = it.name.substr(dot + 1);
      for (auto& c : e.extension) c = std::tolower(c);
    }
    e.mime_type = mime_by_ext(it.name);
  }

  e.type = detect_file_type(it.name, e.is_dir, e.mime_type, full, e.extension);

  // XDG user-directory icons from system theme
  if (e.is_dir) {
    for (int i = 0; i < 8; ++i) {
      if (full == xdg_full[i]) {
        e.icon_name = kXdgIcons[i];
        break;
      }
    }
  }

  if (!e.is_dir && e.extension == "desktop") {
    auto icon = parse_desktop_icon(full);
    if (!icon.empty()) e.icon_name = std::move(icon);
  }

  // Derive Freedesktop icon name from MIME type (media-subtype format)
  if (e.icon_name.empty() && !e.mime_type.empty()) {
    std::string icon = e.mime_type;
    for (auto& c : icon) if (c == '/') c = '-';
    e.icon_name = std::move(icon);
  }

  return e;
}

// Synchronous fallback: fetch metadata with one ::stat per entry.
static FileEntry make_entry(const std::string& prefix, const DirItem& it,
                            const std::unordered_set<std::string>& hidden_names,
                            const std::string xdg_full[8]) {
  struct stat st{};
  StatData sd;
  if (::stat((prefix + it.name).c_str(), &st) == 0) {
    sd.ok = true;
    sd.mode = st.st_mode;
    sd.size = static_cast<uint64_t>(st.st_size);
    sd.mtime_sec = static_cast<int64_t>(st.st_mtime);
    sd.uid = st.st_uid;
    sd.gid = st.st_gid;
  }
  return make_entry_from(it, sd, prefix, hidden_names, xdg_full);
}

// Re-stat a cached entry from disk and refresh its metadata in place so a
// row reflects the current file — size, mtime, permissions, type, mime and
// derived icon — without a full directory reload. Called by the inotify
// watcher when a watched file is modified in place (IN_MODIFY/IN_ATTRIB/
// IN_CLOSE_WRITE), e.g. a binary that was just relinked over itself.
void refresh_entry_from_disk(FileEntry& e) {
  struct stat st{};
  if (::stat(e.path.c_str(), &st) != 0) return;  // gone: structural reload drops it

  e.size = static_cast<uint64_t>(st.st_size);
  e.modified_sec = static_cast<int64_t>(st.st_mtime);
  e.mode = st.st_mode;
  e.readable = (st.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) != 0 ||
               geteuid() == 0;
  e.writable = (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0 ||
               geteuid() == 0;
  e.owner = user_name(st.st_uid);
  e.group = group_name(st.st_gid);
  e.owned_by_root = (st.st_uid == 0);
  e.is_symlink = S_ISLNK(st.st_mode);
  e.is_dir = S_ISDIR(st.st_mode);

  if (e.is_symlink) {
    char buf[4096];
    ssize_t n = readlink(e.path.c_str(), buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      e.link_target = buf;
    } else {
      e.link_target.clear();
    }
  } else {
    e.link_target.clear();
  }

  if (e.is_dir) {
    // Keep the existing (scan-time) icon: folders have no type change to
    // re-derive from, and clearing would drop the XDG-userdir emblems.
    return;
  }

  // Lowercase extension without the dot (mirrors make_entry_from).
  e.extension.clear();
  auto dot = e.name.rfind('.');
  if (dot != std::string::npos && dot + 1 < e.name.size()) {
    e.extension = e.name.substr(dot + 1);
    for (auto& c : e.extension) c = std::tolower(c);
  }

  e.mime_type = mime_by_ext(e.name);
  e.type = detect_file_type(e.name, false, e.mime_type, e.path, e.extension);

  // Re-derive the icon from the fresh MIME (mirrors make_entry_from) so a
  // changed type (text -> ELF binary) gets the right artwork at next paint.
  e.icon_name.clear();
  if (!e.mime_type.empty()) {
    std::string icon = e.mime_type;
    for (auto& c : icon) if (c == '/') c = '-';
    e.icon_name = std::move(icon);
  }
}

// Streaming directory reader: getdents64 batches land in fixed-size chunks
// that build workers consume while the reader is still filling later ones.
// deque so chunk references stay valid while new chunks are appended.
static constexpr size_t kStreamChunk = 4096;
// Don't bother posting progressive skeletons below this many items —
// small directories finish before the first tick anyway.
static constexpr size_t kProgressMinItems = 2000;

struct DirStream {
  std::deque<std::vector<DirItem>> chunks;
  std::atomic<size_t> published{0};   // items fully visible to workers
  std::atomic<bool> read_done{false};

  const DirItem& at(size_t i) const {
    return chunks[i / kStreamChunk][i % kStreamChunk];
  }
};

// Reader pass: bulk getdents64 — one syscall per 64 KB of directory entries.
// Items land in fixed-size chunks; `published` counts how many are fully
// visible so build workers can consume while later batches are still being
// filled. deque keeps element references stable across appends.
static bool read_dir_stream(int dfd, DirStream& stream,
                            const std::atomic<bool>& cancel) {
  trace::set_thread_name("scan-reader");
  const bool tr = trace::enabled().load(std::memory_order_relaxed);
  if (tr) trace::log("reader: start dfd=%d", dfd);
  alignas(16) static thread_local char buf[1 << 16];
  stream.chunks.emplace_back();
  stream.chunks.back().reserve(kStreamChunk);
  size_t total = 0;
  int batches = 0;
  for (;;) {
    if (cancel.load(std::memory_order_relaxed)) {
      if (tr) trace::log("reader: CANCELLED at %zu items after %d batches",
                         total, batches);
      stream.read_done.store(true, std::memory_order_release);
      return false;
    }
    ssize_t n = ::syscall(SYS_getdents64, dfd, buf, sizeof(buf));
    ++batches;
    if (tr && (batches % 50) == 0)
      trace::log("reader: batch %d, %zu items published", batches, total);
    if (n <= 0) {
      if (tr)
        trace::log("reader: done eof=%d after %d batches, %zu items",
                   n == 0, batches, total);
      stream.read_done.store(true, std::memory_order_release);
      return n == 0 || errno == EINTR ? n == 0 : false;
    }
    for (ssize_t off = 0; off < n;) {
      auto* d = reinterpret_cast<struct dirent64*>(buf + off);
      off += d->d_reclen;
      std::string name(d->d_name);
      if (name == "." || name == "..") continue;
      if (stream.chunks.back().size() == kStreamChunk) {
        stream.chunks.emplace_back();
        stream.chunks.back().reserve(kStreamChunk);
      }
      stream.chunks.back().push_back(
          {std::move(name), static_cast<unsigned char>(d->d_type)});
      ++total;
    }
    stream.published.store(total, std::memory_order_release);
  }
}

// One worker's share of a streamed directory: grabs windows (fixed ranges
// of kStreamChunk items) round-robin as soon as the reader has published
// them, and statxes each window in batches through a private io_uring ring
// (dirfd + relative names, so no per-entry path walk).
#if defined(EH_HAVE_IO_URING)
static void statx_stream_worker(int dfd,
                                const DirStream& stream,
                                unsigned worker, unsigned nworkers,
                                const std::string& prefix,
                                const std::unordered_set<std::string>& hidden_names,
                                const std::string xdg_full[8],
                                std::vector<FileEntry>& out,
                                const std::atomic<bool>& cancel) {
  char tname[16];
  std::snprintf(tname, sizeof(tname), "scan-w%u", worker);
  trace::set_thread_name(tname);
  const bool tr = trace::enabled().load(std::memory_order_relaxed);
  constexpr unsigned D = 512;
  io_uring ring;
  bool uring_ok = io_uring_queue_init(D, &ring, 0) == 0;
  if (tr) trace::log("w%u: start uring=%d", worker, (int)uring_ok);
  std::vector<struct statx> stx(uring_ok ? D : 1);

  // Batched statx of one fully-available slice [cur, hi) through our ring.
  auto build_slice = [&](size_t cur, size_t hi) {
    if (!uring_ok) {
      for (size_t i = cur; i < hi; ++i)
        out.push_back(make_entry(prefix, stream.at(i), hidden_names, xdg_full));
      return;
    }
    size_t submitted = cur, reaped = cur;
    while (reaped < hi && !cancel.load(std::memory_order_relaxed)) {
      struct io_uring_sqe* sqe;
      while (submitted < hi &&
             (sqe = io_uring_get_sqe(&ring)) != nullptr) {
        unsigned slot = static_cast<unsigned>(submitted % D);
        io_uring_prep_statx(sqe, dfd, stream.at(submitted).name.c_str(), 0,
                            STATX_BASIC_STATS | STATX_TYPE, &stx[slot]);
        io_uring_sqe_set_data(
            sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(submitted)));
        ++submitted;
      }
      if (io_uring_submit(&ring) < 0) break;

      struct io_uring_cqe* cqe;
      if (io_uring_wait_cqe(&ring, &cqe) != 0) break;

      unsigned head, count = 0;
      io_uring_for_each_cqe(&ring, head, cqe) {
        size_t idx =
            reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));
        out.push_back(make_entry_from(stream.at(idx),
                                      statdata_from_statx(cqe->res,
                                                          stx[idx % D]),
                                      prefix, hidden_names, xdg_full));
        ++count;
      }
      io_uring_cq_advance(&ring, count);
      reaped += count;
    }
    // Leftovers on error paths: finish synchronously.
    for (; reaped < hi; ++reaped) {
      struct statx sx{};
      int res = ::statx(dfd, stream.at(reaped).name.c_str(), 0,
                        STATX_BASIC_STATS | STATX_TYPE, &sx);
      out.push_back(make_entry_from(stream.at(reaped),
                                    statdata_from_statx(res, sx),
                                    prefix, hidden_names, xdg_full));
    }
  };

  size_t wbase = 0;   // first unprocessed window assigned to this worker
  size_t built = 0;
  auto win_t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (cancel.load(std::memory_order_relaxed)) {
      if (tr)
        trace::log("w%u: CANCELLED after %zu windows, %zu entries", worker,
                   wbase, built);
      break;
    }
    // Wait until our next window starts to exist (or the read ended).
    const size_t lo = (wbase * nworkers + worker) * kStreamChunk;
    auto wait_t0 = std::chrono::steady_clock::now();
    for (;;) {
      size_t pub = stream.published.load(std::memory_order_acquire);
      bool done = stream.read_done.load(std::memory_order_acquire);
      if (!done && cancel.load(std::memory_order_relaxed)) return;
      if (pub > lo || (done && pub <= lo)) break;
      std::this_thread::yield();
    }
    double waited = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - wait_t0).count();
    size_t pub = stream.published.load(std::memory_order_acquire);
    bool done = stream.read_done.load(std::memory_order_acquire);
    if (pub <= lo && done) {                 // nothing more will ever come
      if (tr)
        trace::log("w%u: exhausted after %zu windows, %zu entries "
                   "(last wait %.1f ms)", worker, wbase, built, waited);
      break;
    }

    // Drain this window COMPLETELY before advancing. The reader may still
    // be filling its middle; skipping ahead would silently drop items.
    const size_t win_end = lo + kStreamChunk;
    size_t cur = lo;
    while (cur < win_end) {
      if (cancel.load(std::memory_order_relaxed)) break;
      size_t pub2 = stream.published.load(std::memory_order_acquire);
      bool done2 = stream.read_done.load(std::memory_order_acquire);
      size_t hi = std::min(pub2, win_end);
      if (hi <= cur) {
        if (done2) break;                       // directory ended mid-window
        std::this_thread::yield();
        continue;
      }
      build_slice(cur, hi);
      built += hi - cur;
      cur = hi;
    }
    ++wbase;
    if (tr && (wbase % 8) == 0)
      trace::log("w%u: %zu windows, %zu entries, last %.1f ms", worker,
                 wbase, built,
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - win_t0).count());
    win_t0 = std::chrono::steady_clock::now();
  }

  if (uring_ok) io_uring_queue_exit(&ring);
}
#endif

// Portable fallback worker: one dirfd-relative statx per entry.
[[maybe_unused]] static void statx_stream_plain(const DirStream& stream,
                                unsigned worker, unsigned nworkers,
                                const std::string& prefix,
                                const std::unordered_set<std::string>& hidden_names,
                                const std::string xdg_full[8],
                                std::vector<FileEntry>& out,
                                const std::atomic<bool>& cancel) {
  char tname[16];
  std::snprintf(tname, sizeof(tname), "scan-p%u", worker);
  trace::set_thread_name(tname);
  for (size_t wbase = 0;; ++wbase) {
    if (cancel.load(std::memory_order_relaxed)) return;
    size_t lo = (wbase * nworkers + worker) * kStreamChunk;
    for (;;) {
      size_t pub = stream.published.load(std::memory_order_acquire);
      bool done = stream.read_done.load(std::memory_order_acquire);
      if (pub > lo || (done && pub <= lo)) break;
      std::this_thread::yield();
    }
    size_t pub = stream.published.load(std::memory_order_acquire);
    bool done = stream.read_done.load(std::memory_order_acquire);
    if (pub <= lo && done) return;
    // Drain the window completely before advancing.
    const size_t win_end = lo + kStreamChunk;
    size_t cur = lo;
    while (cur < win_end) {
      if (cancel.load(std::memory_order_relaxed)) return;
      size_t p = stream.published.load(std::memory_order_acquire);
      bool d = stream.read_done.load(std::memory_order_acquire);
      size_t hi = std::min(p, win_end);
      if (hi <= cur) {
        if (d) break;
        std::this_thread::yield();
        continue;
      }
      for (size_t i = cur; i < hi; ++i) {
#if defined(EH_HAVE_IO_URING)
        struct statx sx{};
        int res = ::statx(0, (prefix + stream.at(i).name).c_str(), 0,
                          STATX_BASIC_STATS | STATX_TYPE, &sx);
        out.push_back(make_entry_from(stream.at(i), statdata_from_statx(res, sx),
                                      prefix, hidden_names, xdg_full));
#else
        out.push_back(make_entry(prefix, stream.at(i), hidden_names, xdg_full));
#endif
      }
      cur = hi;
    }
  }
}

// Chunked parallel sort for very large listings.
//
// For the default Name+natural+case-insensitive ordering we precompute a
// byte-comparable key per entry and sort an index permutation — comparisons
// become memcmp-speed instead of re-parsing names on every call. Other
// fields keep the direct comparator. Merges run as a pairwise tree so every
// level uses multiple threads.
static void sort_entries_parallel(std::vector<FileEntry>& entries,
                                  const ScanParams& sp, unsigned K) {
  const size_t n = entries.size();
  if (n < 2) return;
  if (K > n / 4096) K = std::max(1u, static_cast<unsigned>(n / 4096));
  if (K <= 1) {
    sort_entries(entries, sp);
    return;
  }

  const bool name_mode = static_cast<SortField>(sp.sort_field) == SortField::Name &&
                         sp.natural && !sp.case_sensitive;

  if (!name_mode) {
    const size_t chunk = (n + K - 1) / K;
    {
      std::vector<std::thread> pool;
      pool.reserve(K);
      for (unsigned k = 0; k < K; ++k) {
        size_t lo = k * chunk;
        if (lo >= n) break;
        size_t hi = std::min(n, lo + chunk);
        pool.emplace_back([&entries, lo, hi, &sp] {
          std::sort(entries.begin() + static_cast<long>(lo),
                    entries.begin() + static_cast<long>(hi), EntryLess{&sp, std::time(nullptr)});
        });
      }
      for (auto& t : pool) t.join();
    }
    auto b = entries.begin();
    for (unsigned k = 1; k < K; ++k) {
      size_t cut = k * chunk;
      if (cut >= n) break;
      std::inplace_merge(b, b + static_cast<long>(cut),
                         entries.end(), EntryLess{&sp, std::time(nullptr)});
    }
    return;
  }

  // ── Key-based path ──
  std::vector<std::string> keys(n);
  {
    const size_t chunk = (n + K - 1) / K;
    std::vector<std::thread> pool;
    pool.reserve(K);
    for (unsigned k = 0; k < K; ++k) {
      size_t lo = k * chunk;
      if (lo >= n) break;
      size_t hi = std::min(n, lo + chunk);
      pool.emplace_back([&entries, &keys, lo, hi] {
        for (size_t i = lo; i < hi; ++i)
          natural_key(entries[i].name, keys[i]);
      });
    }
    for (auto& t : pool) t.join();
  }

  EntryLess less{&sp, std::time(nullptr)};

  std::vector<uint32_t> idx(n);
  for (size_t i = 0; i < n; ++i) idx[i] = static_cast<uint32_t>(i);

  auto idx_less = [&](uint32_t a, uint32_t b) {
    const FileEntry& ea = entries[a];
    const FileEntry& eb = entries[b];
    if (sp.folders_first && ea.is_dir != eb.is_dir) return ea.is_dir;
    if (sp.hidden_last && ea.is_hidden != eb.is_hidden) return !ea.is_hidden;
    if (sp.group_field > 0) {
      int ga = less.group_rank(ea), gb = less.group_rank(eb);
      if (ga != gb) return ga < gb;
    }
    int c = keys[a].compare(keys[b]);
    if (c != 0) return c < 0;
    return false;
  };

  const size_t chunk = (n + K - 1) / K;

  // Sort chunks in parallel, then merge pairwise levels in parallel.
  std::vector<std::pair<size_t, size_t>> runs;   // [lo,hi) sorted spans
  runs.reserve(K);
  {
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < K; ++k) {
      size_t lo = k * chunk;
      if (lo >= n) break;
      size_t hi = std::min(n, lo + chunk);
      runs.emplace_back(lo, hi);
      pool.emplace_back([&idx, lo, hi, &idx_less] {
        std::sort(idx.begin() + static_cast<long>(lo),
                  idx.begin() + static_cast<long>(hi), idx_less);
      });
    }
    for (auto& t : pool) t.join();
  }
  while (runs.size() > 1) {
    std::vector<std::pair<size_t, size_t>> next;
    next.reserve((runs.size() + 1) / 2);
    std::vector<std::thread> pool;
    for (size_t r = 0; r + 1 < runs.size(); r += 2) {
      auto [loA, hiA] = runs[r];
      auto [loB, hiB] = runs[r + 1];
      next.emplace_back(loA, hiB);
      pool.emplace_back([&idx, loA, hiA, hiB, &idx_less] {
        std::inplace_merge(idx.begin() + static_cast<long>(loA),
                           idx.begin() + static_cast<long>(hiA),
                           idx.begin() + static_cast<long>(hiB), idx_less);
      });
    }
    if (runs.size() & 1) next.push_back(runs.back());
    for (auto& t : pool) t.join();
    runs.swap(next);
  }

  // Apply the permutation.
  std::vector<FileEntry> sorted;
  sorted.reserve(n);
  for (size_t i = 0; i < n; ++i)
    sorted.push_back(std::move(entries[idx[i]]));
  entries.swap(sorted);
}

static void scan_entries(
    const std::string& dir_path, const ScanParams& sp,
    std::vector<FileEntry>& out, const std::atomic<bool>& cancel,
    const std::function<void(std::vector<FileEntry>)>& on_progress = {}) {
  static const bool bench = [] {
    const char* e = std::getenv("EH_BENCH");
    return e && *e && e[0] != '0';
  }();
  auto t0 = std::chrono::steady_clock::now();

  int dfd = ::open(dir_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) return;

  const std::unordered_set<std::string> hidden_names =
      read_hidden_file(dir_path);

  // XDG user-directory paths resolved once per scan; shared read-only.
  const std::string xdg_home = home_dir();
  std::string xdg_full[8];
  for (int i = 0; i < 8; ++i) xdg_full[i] = xdg_home + kXdgSubdirs[i];

  std::string prefix = dir_path + "/";
  prefix.shrink_to_fit();

  DirStream stream;
  auto t_read1 = t0;
  unsigned workers = 1;
  std::atomic<bool> used_uring{false};
  auto t_built1 = t0;

  trace::set_thread_name("scan-main");
  if (trace::enabled().load(std::memory_order_relaxed))
    trace::log("scan: start '%s' gen-work", dir_path.c_str());

  {
    // Reader runs concurrently with the build workers: they start statxing
    // the first published chunks while later batches are still being read.
    std::thread reader([&, dfd]() { read_dir_stream(dfd, stream, cancel); });

    // Progressive feed (Dolphin-style): while statx workers are still
    // chewing, post name-only skeletons of everything readdir has already
    // published. The view fills with rows long before metadata lands;
    // the final READY result replaces them with fully-populated entries.
    std::thread ticker;
    if (on_progress) {
      trace::set_thread_name("scan-ticker");
      ticker = std::thread([&stream, &sp, &on_progress, &cancel, &prefix]() {
        // Dolphin policy (KFileItemModel): show one early partial view,
        // then leave the UI alone for 2 s while metadata lands — repeated
        // full-list rebuilds steal CPU from the statx workers.
        constexpr double kFirstPostMs = 50.0;
        constexpr double kLaterPostMs = 2000.0;
        auto last = std::chrono::steady_clock::now();
        bool posted_once = false;
        for (;;) {
          std::this_thread::sleep_for(std::chrono::milliseconds(40));
          if (cancel.load(std::memory_order_relaxed)) return;
          size_t pub = stream.published.load(std::memory_order_acquire);
          // Reader finished? The fully-populated READY result lands within
          // milliseconds — stop feeding skeletons (also prevents the
          // small-directory case from waiting on a threshold it never hits).
          if (stream.read_done.load(std::memory_order_acquire)) return;
          if (pub < kProgressMinItems) continue;
          double since = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - last)
                             .count();
          double need = posted_once ? kLaterPostMs : kFirstPostMs;
          if (since < need) continue;
          last = std::chrono::steady_clock::now();
          posted_once = true;
          std::vector<FileEntry> skel;
          skel.reserve(pub);
          for (size_t i = 0; i < pub; ++i) {
            const DirItem& it = stream.at(i);
            FileEntry& e = skel.emplace_back();
            e.name = it.name;
            e.path = prefix;
            e.path += it.name;
            e.is_dir = (it.dtype == DT_DIR);
            e.is_hidden = is_hidden_file(it.name);
          }
          sort_entries_parallel(skel, sp, 4u);
          on_progress(std::move(skel));
        }
      });
    }
    auto wait_readable = [&] {
      while (!stream.read_done.load(std::memory_order_acquire) &&
             stream.published.load(std::memory_order_acquire) < 512)
        std::this_thread::yield();
    };
    wait_readable();

    if (stream.read_done.load(std::memory_order_acquire)) {
      // Small directory: everything arrived already — build inline.
      if (trace::enabled().load(std::memory_order_relaxed))
        trace::log("scan: small-dir path (%zu published)",
                   stream.published.load(std::memory_order_acquire));
      reader.join();
      const size_t n = stream.published.load(std::memory_order_acquire);
      t_read1 = std::chrono::steady_clock::now();
      t_built1 = t_read1;
      out.reserve(n);
      for (size_t i = 0; i < n; ++i)
        out.push_back(make_entry(prefix, stream.at(i), hidden_names, xdg_full));
    } else {
      unsigned hw = std::thread::hardware_concurrency();
      workers = std::clamp(hw ? hw : 2u, 2u, 16u);
      if (trace::enabled().load(std::memory_order_relaxed))
        trace::log("scan: parallel path, %u workers", workers);
      std::vector<std::vector<FileEntry>> parts(workers);
      std::vector<std::thread> pool;
      pool.reserve(workers);
      for (unsigned k = 0; k < workers; ++k) {
        pool.emplace_back([&, k]() {
#if defined(EH_HAVE_IO_URING)
          used_uring.store(true, std::memory_order_relaxed);
          statx_stream_worker(dfd, stream, k, workers, prefix, hidden_names,
                              xdg_full, parts[k], cancel);
#else
          statx_stream_plain(stream, k, workers, prefix, hidden_names,
                             xdg_full, parts[k], cancel);
#endif
        });
      }
      reader.join();   // reading done; keep this thread free for merges
      t_read1 = std::chrono::steady_clock::now();
      for (auto& th : pool) th.join();
      t_built1 = std::chrono::steady_clock::now();
      {
        const bool tr = trace::enabled().load(std::memory_order_relaxed);
        double read_ms =
            std::chrono::duration<double, std::milli>(t_read1 - t0).count();
        double build_ms =
            std::chrono::duration<double, std::milli>(t_built1 - t_read1)
                .count();
        if (tr)
          trace::log("scan: reader %.1f ms, workers joined in %.1f ms "
                     "(overlap saved ~%.0f%%)",
                     read_ms, build_ms,
                     read_ms > 0 ? 100.0 * (1.0 - build_ms / read_ms) : 0.0);
      }

      size_t total = 0;
      for (const auto& part : parts) total += part.size();
      out.reserve(out.size() + total);
      for (auto& part : parts)
        out.insert(out.end(), std::make_move_iterator(part.begin()),
                   std::make_move_iterator(part.end()));
    }
    if (ticker.joinable()) ticker.join();
  }
  ::close(dfd);

  auto t1 = std::chrono::steady_clock::now();

  if (trace::enabled().load(std::memory_order_relaxed))
    trace::log("scan: merging %zu entries from %u parts", out.size(), workers);
  sort_entries_parallel(out, sp, workers > 8 ? 8u : workers);
  if (trace::enabled().load(std::memory_order_relaxed) && !out.empty())
    trace::log("scan: first='%s' last='%s'", out.front().name.c_str(),
               out.back().name.c_str());

  if (bench) {
    double read_ms =
        std::chrono::duration<double, std::milli>(t_read1 - t0).count();
    double build_ms = std::chrono::duration<double, std::milli>(
        t_built1 - t_read1).count();
    double sort_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t1).count();
    fprintf(stderr,
            "[perf] bg_scan '%s' n=%zu read=%.1f build=%.1f sort=%.1f "
            "w=%u %s\n",
            dir_path.c_str(), out.size(), read_ms, build_ms, sort_ms,
            workers, used_uring.load() ? "io_uring" : "stat");
  }
}

// Pin cur_tab() to the pane that requested the in-flight scan for the
// duration of an apply, so helpers inside (and build_tree_entries) resolve
// to the target pane even if focus sits on the other split pane.
struct ScanPaneGuard {
  AppState& app;
  int saved;
  explicit ScanPaneGuard(AppState& a)
      : app(a), saved(a.active_pane) {
    if (a.split_view && a.scan_target_pane == 1) a.active_pane = 1;
  }
  ~ScanPaneGuard() { app.active_pane = saved; }
};

// Swap a finished background scan into the active tab. Runs on the UI
// thread — either inline from reload_dir's fast path or polled from the
// main loop once the worker finishes.
void apply_scan_result(AppState& app, bool is_progress) {
  ScanPaneGuard pane_guard(app);
  const bool tr = trace::enabled().load(std::memory_order_relaxed);
  if (tr)
    trace::log("apply: begin ready=%d progress=%d n=%zu",
               (int)app.scan_ready_flag.load(std::memory_order_acquire),
               (int)is_progress, app.scan_result.entries.size());
  static const bool bench = [] {
    const char* e = std::getenv("EH_BENCH");
    return e && *e && e[0] != '0';
  }();
  auto t_apply0 = std::chrono::steady_clock::now();

  std::vector<FileEntry> incoming;
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    if (app.scan_result.generation != app.scan_generation) {
      app.scan_ready_flag.store(false);
      return;
    }
    incoming.swap(app.scan_result.entries);
    if (!is_progress) app.scan_result.generation = 0;
  }
  if (is_progress) {
    app.scan_progress_flag.store(false, std::memory_order_release);
  } else {
    app.scan_ready_flag.store(false);
    app.scan_apply_deferred = false;
    app.scan_active_path.clear();
    app.operation_status.clear();
    app.operation_status_expires_ms = 0;
  }

  auto& tab = app.cur_tab();
  const bool had_content = !tab.entries.empty();
  std::string prev_sel_name;
  if (had_content && tab.selected_idx >= 0 &&
      tab.selected_idx < static_cast<int>(tab.visible_entries.size())) {
    int ri = tab.visible_entries[tab.selected_idx];
    if (ri >= 0 && ri < static_cast<int>(tab.entries.size()))
      prev_sel_name = tab.entries[ri].name;
  }

  tab.entries = std::move(incoming);
  tab.tree_entries_dirty = true;
  ++app.listing_epoch;  // scroll-delta content reuse invalid

  tab.visible_entries.clear();
  tab.visible_entries.reserve(tab.entries.size());
  for (int i = 0; i < static_cast<int>(tab.entries.size()); ++i) {
    if (!app.show_hidden && tab.entries[i].is_hidden) continue;
    if (!query_matches_entry(app, tab.entries[i].name)) continue;
    tab.visible_entries.push_back(i);
  }

  recompute_item_counts(tab, app.show_hidden);  // O(1) status-bar aggregates

  if (had_content && !prev_sel_name.empty()) {
    // Refresh of an already-visible folder: keep selection and scroll.
    for (int vi = 0; vi < static_cast<int>(tab.visible_entries.size()); ++vi) {
      if (tab.entries[tab.visible_entries[vi]].name == prev_sel_name) {
        tab.selected_idx = vi;
        break;
      }
    }
  } else {
    reset_scroll_and_selection(app);
  }

  // Track directory mtime for auto-refresh
  struct stat dir_st;
  if (::stat(tab.current_path.c_str(), &dir_st) == 0)
    tab.dir_mtime = static_cast<int64_t>(dir_st.st_mtime);

  // ── Dynamic view: media-heavy folders auto-switch to icon view once ──
  // Mirrors Dolphin's applyDynamicView: images/videos must outweigh all other
  // entries 2:1, with each subdirectory only counting a third.
  {
    bool searching = app.search_active || app.recursive_search_active ||
                     app.r_search_active || app.r_recursive_search_active;
    if (app.dynamic_view && !searching && tab.view_mode == ViewMode::List &&
        !tab.dynamic_view_done && !tab.entries.empty()) {
      int media = 0, dirs = 0, others = 0;
      std::string first_media;
      for (const auto& e : tab.entries) {
        if (!app.show_hidden && e.is_hidden) continue;
        if (e.type == FileType::Image || e.type == FileType::Video) {
          ++media;
          if (first_media.empty()) first_media = e.path;
        } else if (e.is_dir) {
          ++dirs;
        } else {
          ++others;
        }
      }
      double non_media_weight = static_cast<double>(others) +
                                static_cast<double>(dirs) / 3.0;
      if (media > 0 &&
          static_cast<double>(media) >= 2.0 * non_media_weight) {
        tab.view_mode = ViewMode::Grid;
        app.last_browser_view_mode = ViewMode::Grid;
        tab.dynamic_view_done = true;
        app.media_folder_child[tab.current_path] = first_media;
      }
    }
  }

  // Clear pending thumbnail queue since entry list changed
  app.thumb_pending_queue.clear();

  if (!is_progress) {
    // Populate background pre-cache queue with all image file paths.
    // Skipped for progressive skeletons: they carry no type info yet.
    app.precache_paths.clear();
    for (const auto& e : tab.entries) {
      if (e.type == FileType::Image || e.type == FileType::Video) {
        app.precache_paths.push_back(e.path);
      }
    }
    app.precache_idx = 0;
  }

  // Rebuild tree entries if in tree mode
  if (tab.view_mode == ViewMode::Tree) build_tree_entries(app);

  if (is_progress) {
    std::string leaf =
        fs::path(tab.current_path).filename().string();
    app.operation_status = "Loading " +
        (leaf.empty() ? tab.current_path : leaf) + " … " +
        std::to_string(tab.entries.size());
    app.operation_status_expires_ms = 0;   // sticky until final apply
  }

  if (bench)
    fprintf(stderr, "[perf] apply%s n=%zu visible=%zu %.1f ms\n",
            is_progress ? "-progress" : "",
            tab.entries.size(), tab.visible_entries.size(),
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_apply0).count());

  // Final scan landed: resolve all visible icons synchronously so the next
  // paint is final artwork — no placeholder flash on folder open/switch.
  if (!is_progress) prewarm_tab_icons(app);
}

static void recompute_item_counts(Tab& tab, bool show_hidden) {
  int files = 0, dirs = 0;
  for (const auto& e : tab.entries) {
    if (!show_hidden && e.is_hidden) continue;
    if (e.is_dir) ++dirs;
    else ++files;
  }
  tab.cached_total_items = files + dirs;
  tab.cached_total_files = files;
  tab.cached_total_dirs = dirs;
}

void thumb_cache_install(AppState& app, const std::string& path, int size,
                         cairo_surface_t* s) {
  if (!s) return;
  auto dup = app.thumb_cache.find(path);
  if (dup != app.thumb_cache.end()) {
    // Already have one — keep the larger decode (e.g. a hover-preview's
    // hi-res request replacing the grid's low-res thumb).
    int ew = cairo_image_surface_get_width(dup->second);
    int eh = cairo_image_surface_get_height(dup->second);
    int nw = cairo_image_surface_get_width(s);
    int nh = cairo_image_surface_get_height(s);
    if (std::max(nw, nh) > std::max(ew, eh)) {
      cairo_surface_destroy(dup->second);
      dup->second = s;
      app.thumb_lru.push_front(path);
    } else {
      cairo_surface_destroy(s);
    }
    (void)size;
    return;
  }
  app.thumb_cache[path] = s;
  app.thumb_lru.push_front(path);
  (void)size;
}

// Cancel + reap any in-flight background scan.
void join_scan(AppState& app) {
  if (trace::enabled().load(std::memory_order_relaxed))
    trace::log("join_scan: cancelling gen=%zu active='%s'",
               app.scan_generation, app.scan_active_path.c_str());
  app.scan_cancel.store(true);
  if (app.scan_thread.joinable()) app.scan_thread.join();
  app.scan_generation++;  // invalidate any result the worker may have posted
  app.scan_progress_flag.store(false, std::memory_order_release);
  if (trace::enabled().load(std::memory_order_relaxed))
    trace::log("join_scan: done, gen now %zu", app.scan_generation);
  app.scan_generation++;  // invalidate any result the worker may have posted
  app.scan_ready_flag.store(false);
  app.scan_active_path.clear();
}

void reload_dir(AppState& app) {
  // Perf instrumentation: EH_BENCH=1 prints per-reload timings.
  static const bool bench = [] {
    const char* e = std::getenv("EH_BENCH");
    return e && *e && e[0] != '0';
  }();

  // Remember the requesting pane up front so a deferred scan applies back
  // to it even if focus moves to the other split pane in the meantime.
  app.scan_target_pane = app.active_pane ? 1 : 0;

  reset_preview(app);
  hide_tooltip(app);

  // Virtual computer view — no directory to load
  if (app.cur_tab().current_path == "computer://") {
    app.cur_tab().view_mode = ViewMode::Computer;
    app.computer_needs_refresh = true;
    return;
  }

  if (!fs::is_directory(app.cur_tab().current_path)) {
    app.cur_tab().current_path = home_dir();
  }

  // Reset view mode if coming from computer view
  if (app.cur_tab().view_mode == ViewMode::Computer) {
    app.cur_tab().view_mode = app.last_browser_view_mode;
  }

  // ── Per-folder .directory view properties ──
  app.cur_tab().dynamic_view_done = false;
  if (app.per_folder_props) {
    DirProps dp;
    if (read_dir_props(app.cur_tab().current_path, &dp)) {
      auto& tab = app.cur_tab();
      if (dp.has_mode && dp.mode >= 0 && dp.mode <= 4 &&
          dp.mode != static_cast<int>(ViewMode::Computer)) {
        tab.view_mode = static_cast<ViewMode>(dp.mode);
        app.last_browser_view_mode = tab.view_mode;
      }
      if (dp.has_sort && dp.sort_field >= 0 &&
          dp.sort_field <= static_cast<int>(SortField::LinkTarget)) {
        tab.sort_field = static_cast<SortField>(dp.sort_field);
        tab.sort_descending = dp.sort_descending;
      }
      if (dp.has_flags) {
        app.sort_natural = dp.natural;
        app.sort_case_sensitive = dp.case_sensitive;
        app.sort_hidden_last = dp.hidden_last;
        app.folders_before_files = dp.folders_first;
      }
      if (dp.has_group && dp.group_field >= 0 && dp.group_field <= 4) {
        tab.group_field = dp.group_field;
        tab.group_by_type = (dp.group_field == 1);
      }
      if (dp.has_zoom && dp.zoom_level >= 0 && dp.zoom_level < kZoomLevelCount)
        apply_zoom_pct(app, zoom_pct_for_level(dp.zoom_level));
      if (dp.has_hidden) app.show_hidden = dp.show_hidden;
    }
  }

  // ── Launch the scan on a background thread ──
  std::string scan_path = fs::path(app.cur_tab().current_path).lexically_normal().string();
  if (!scan_path.empty() && scan_path != app.cur_tab().current_path)
    app.cur_tab().current_path = scan_path;   // collapse '//', trailing '/'

  // Claim the directory's mtime up front so the frame loop's auto-refresh
  // doesn't re-fire reload_dir every frame while a deferred scan is pending.
  {
    struct stat dir_st;
    if (::stat(scan_path.c_str(), &dir_st) == 0)
      app.cur_tab().dir_mtime = static_cast<int64_t>(dir_st.st_mtime);
  }

  ScanParams sp;
  sp.folders_first  = app.folders_before_files;
  sp.hidden_last    = app.sort_hidden_last;
  sp.natural        = app.sort_natural;
  sp.case_sensitive = app.sort_case_sensitive;
  sp.group_field    = app.cur_tab().group_field;
  sp.sort_field     = static_cast<int>(app.cur_tab().sort_field);
  sp.descending     = app.cur_tab().sort_descending;

  // Identical scan already running? Let it finish instead of restarting
  // from zero — repeated navigation to the same folder stays free.
  const bool reusable =
      app.scan_thread.joinable() &&
      !app.scan_ready_flag.load(std::memory_order_acquire) &&
      app.scan_active_path == scan_path && app.scan_active_params == sp;

  if (trace::enabled().load(std::memory_order_relaxed))
    trace::log("reload_dir: '%s' reusable=%d gen=%zu", scan_path.c_str(),
               (int)reusable, app.scan_generation);
  if (reusable && bench)
    fprintf(stderr, "[perf] reload_dir '%s' joined in-flight scan\n",
            scan_path.c_str());
  if (!reusable) {
    join_scan(app);

  const uint64_t gen = ++app.scan_generation;
  app.scan_cancel.store(false);
  app.scan_ready_flag.store(false);
  app.scan_active_path = scan_path;
  app.scan_active_params = sp;
  if (trace::enabled().load(std::memory_order_relaxed))
    trace::log("reload_dir: launching '%s' gen=%llu", scan_path.c_str(),
               (unsigned long long)gen);
  AppState* ap = &app;
  app.scan_thread = std::thread([ap, gen, sp, scan_path]() {
    trace::set_thread_name("scan-main");
    std::vector<FileEntry> v;
    auto on_progress = [ap, gen, scan_path](std::vector<FileEntry> skel) {
      std::lock_guard<std::mutex> lk(ap->scan_mtx);
      if (ap->scan_generation != gen || ap->scan_cancel.load()) return;
      ap->scan_result.generation = gen;
      ap->scan_result.path = scan_path;
      ap->scan_result.entries = std::move(skel);
      ap->scan_progress_flag.store(true, std::memory_order_release);
    };
    scan_entries(scan_path, sp, v, ap->scan_cancel, on_progress);
    std::lock_guard<std::mutex> lk(ap->scan_mtx);
    if (ap->scan_generation != gen || ap->scan_cancel.load()) {
      if (trace::enabled().load(std::memory_order_relaxed))
        trace::log("scan: result DISCARDED for '%s' (stale/cancelled)",
                   scan_path.c_str());
      ap->scan_progress_flag.store(false, std::memory_order_release);
      return;
    }
    if (trace::enabled().load(std::memory_order_relaxed))
      trace::log("scan: result READY '%s' n=%zu", scan_path.c_str(),
                 v.size());
    ap->scan_result.generation = gen;
    ap->scan_result.path = scan_path;
    ap->scan_result.entries = std::move(v);
    ap->scan_ready_flag.store(true);
    ap->scan_progress_flag.store(false, std::memory_order_release);
  });
  }

  // Fast path: normal dirs finish in well under a frame — apply inline so
  // navigation feels identical to the old synchronous load.
  constexpr double kScanFastPathMs = 40.0;
  auto t_launch = std::chrono::steady_clock::now();
  // Cold start: never block the first frame on a scan that won't beat the
  // timer anyway — defer straight to the background path.
  if (app.startup_loading &&
      !app.scan_ready_flag.load(std::memory_order_acquire)) {
    app.scan_apply_deferred = true;
    if (trace::enabled().load(std::memory_order_relaxed))
      trace::log("reload_dir: startup defer '%s'", scan_path.c_str());
    return;
  }
  while (!app.scan_ready_flag.load(std::memory_order_acquire)) {
    double waited_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t_launch).count();
    if (waited_ms > kScanFastPathMs) {
      app.scan_apply_deferred = true;
      // Instant feedback: acknowledge the navigation right now (blank view +
      // status line) so big folders never feel frozen while scanning.
      auto& tab = app.cur_tab();
      tab.entries.clear();
      tab.visible_entries.clear();
      ++app.listing_epoch;  // scroll-delta content reuse invalid
      tab.selected_idx = -1;
      tab.hover_idx = -1;
      tab.multi_selected.clear();
      std::string leaf = fs::path(scan_path).filename().string();
      app.operation_status =
          "Loading " + (leaf.empty() ? scan_path : leaf) + " …";
      app.operation_status_expires_ms = 0;   // sticky until apply clears it
      app.pendingRedraw = true;
      static const bool bench = [] {
        const char* e = std::getenv("EH_BENCH");
        return e && *e && e[0] != '0';
      }();
      if (bench)
        fprintf(stderr, "[perf] reload_dir '%s' deferred to background\n",
                scan_path.c_str());
      if (trace::enabled().load(std::memory_order_relaxed))
        trace::log("reload_dir: DEFERRED '%s' to background after %.1f ms",
                   scan_path.c_str(), waited_ms);
      return;   // frame loop applies results when ready
    }
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }

  apply_scan_result(app);
}

} // namespace eh::file_browser
