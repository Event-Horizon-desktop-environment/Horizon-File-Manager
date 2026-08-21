#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace eh::file_browser {

struct SearchResult {
  std::string path;
  std::string relative_path; // relative to search root
  bool is_dir = false;
};

// Options for a recursive search run.
struct SearchOptions {
  int mode = 0;                // QueryMode: 0=plain, 1=glob, 2=regex, 3=content
  bool case_sensitive = false;

  // Optional predicate applied in the worker thread (prunes results early).
  // Return false to exclude the entry. Receives stat data (size/mtime are
  // 0 when the stat failed).
  std::function<bool(const std::string& path, const std::string& name,
                     bool is_dir, uint64_t size, int64_t mtime)>
      predicate;
};

class RecursiveSearchWorker {
public:
  RecursiveSearchWorker();
  ~RecursiveSearchWorker();

  /// Start a new recursive search from root_dir. Cancels any in-flight search.
  void start_search(const std::string& root_dir, const std::string& query,
                    const SearchOptions& options = {});

  /// Poll available results. Returns false when no more results in queue.
  bool poll(SearchResult& out);

  /// Returns true if a search is currently running.
  bool busy();

  /// Cancel any running search and clear pending results.
  void cancel();

private:
  void thread_main();
  void walk_directory(const std::string& dir, const std::string& rel,
                      int depth);

  bool match_name(const std::string& name);
  bool match_content(const std::string& path);

  std::thread m_thread;
  std::mutex m_out_mutex;
  std::queue<SearchResult> m_out;

  std::mutex m_ctrl_mutex;
  std::string m_root_dir;
  std::string m_query;
  SearchOptions m_options;
  std::regex m_regex;
  bool m_regex_ok = false;
  bool m_search_pending = false;
  bool m_cancel_requested = false;

  std::condition_variable m_cv;
  std::atomic<bool> m_running{true};
};

RecursiveSearchWorker& recursive_search_worker();

} // namespace eh::file_browser
