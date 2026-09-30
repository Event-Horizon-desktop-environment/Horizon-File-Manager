#pragma once

// grep.hpp — horizon-grep: parallel literal file search.
//
// Design (ripgrep-shaped, no regex in v1):
// - one fast walker thread feeds file paths into a bounded queue;
// - N search workers read files (mmap for large, read() for small),
//   skip binaries, and match with SIMD-backed memmem (exact) or a
//   case-folded Horspool (-i);
// - .gitignore/.ignore + hidden-file pruning happens in the walker, so
//   workers never touch skipped trees (this is most of the speed).
//
// The file browser's future search engine drives hgrep_search() on a
// worker thread; the horizon-grep CLI wraps the same core for
// benchmarking and scripting.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hgrep {

struct GrepOptions {
  std::string pattern;        // literal substring (UTF-8 opaque bytes)
  bool case_insensitive = false;
  bool hidden = false;        // search dotfiles/dirs (default: skip)
  bool no_ignore = false;     // ignore .gitignore/.ignore (default: respect)
  bool follow_symlink_dirs = false;
  uint64_t max_filesize = 50u * 1024u * 1024u; // larger files are skipped
  int threads = 0;            // 0 = measured default (cores/3, 4..8)
  bool files_only = false;    // -l: report paths, skip line extraction
  bool count_only = false;    // -c: report per-file match counts
};

struct GrepMatch {
  std::string path;
  uint64_t line_no = 0;       // 1-based (0 when files_only, or binary hit)
  uint64_t byte_offset = 0;   // offset of match in file
  std::string line;           // matching line, '\n' stripped ("" if files_only
                              // or binary hit)
};

struct GrepStats {
  uint64_t files_searched = 0;
  uint64_t files_skipped = 0; // too big, unreadable, ignored…
  uint64_t bytes_searched = 0;
  uint64_t matches = 0;
};

// Search `root` recursively. on_match returns false to stop early.
// Safe to call off the UI thread; `cancel` is polled between files.
// Returns false only on fatal walker errors (bad root, …).
bool hgrep_search(const std::string& root, const GrepOptions& opt,
                  const std::function<bool(GrepMatch)>& on_match,
                  const std::atomic<bool>& cancel, GrepStats& stats);

} // namespace hgrep
