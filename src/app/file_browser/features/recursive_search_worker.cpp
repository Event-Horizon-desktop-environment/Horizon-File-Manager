#include "app/file_browser/features/recursive_search_worker.hpp"
#include "app/file_browser/features/query_match.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <fnmatch.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace eh::file_browser {
namespace {

constexpr size_t kContentMaxBytes = 512 * 1024;  // never grep beyond 512 KB
constexpr size_t kBinaryProbeBytes = 8192;

bool is_skip_dir(const std::string& name) {
  return name == "." || name == ".." || name == "snap" ||
         name == "lost+found";
}

bool plain_contains(const std::string& haystack, const std::string& needle,
                    bool case_sensitive) {
  if (case_sensitive) return haystack.find(needle) != std::string::npos;
  auto fold = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
  };
  return fold(haystack).find(fold(needle)) != std::string::npos;
}

// True when the head of the file looks textual (no NUL bytes in probe).
bool looks_textual(const char* buf, size_t n) {
  return std::memchr(buf, '\0', n) == nullptr;
}

} // namespace

RecursiveSearchWorker::RecursiveSearchWorker()
    : m_thread(&RecursiveSearchWorker::thread_main, this) {}

RecursiveSearchWorker::~RecursiveSearchWorker() {
  cancel();
  m_running.store(false);
  m_cv.notify_one();
  if (m_thread.joinable()) m_thread.join();
}

void RecursiveSearchWorker::start_search(const std::string& root_dir,
                                          const std::string& query,
                                          const SearchOptions& options) {
  // Clear remaining results from previous search
  {
    std::lock_guard<std::mutex> lock(m_out_mutex);
    std::queue<SearchResult> empty;
    std::swap(m_out, empty);
  }
  {
    std::lock_guard<std::mutex> lock(m_ctrl_mutex);
    m_root_dir = root_dir;
    m_query = query;
    m_options = options;
    m_regex_ok = false;
    if (options.mode == static_cast<int>(QueryMode::Regex) &&
        !query.empty()) {
      try {
        auto flags = std::regex::ECMAScript;
        if (!options.case_sensitive) flags |= std::regex::icase;
        m_regex.assign(query, flags);
        m_regex_ok = true;
      } catch (const std::regex_error&) {
        m_regex_ok = false;
      }
    }
    m_search_pending = true;
    m_cancel_requested = false;
  }
  m_cv.notify_one();
}

bool RecursiveSearchWorker::poll(SearchResult& out) {
  std::lock_guard<std::mutex> lock(m_out_mutex);
  if (m_out.empty()) return false;
  out = std::move(m_out.front());
  m_out.pop();
  return true;
}

bool RecursiveSearchWorker::busy() {
  std::lock_guard<std::mutex> lock(m_ctrl_mutex);
  return m_search_pending;
}

void RecursiveSearchWorker::cancel() {
  {
    std::lock_guard<std::mutex> lock(m_ctrl_mutex);
    m_cancel_requested = true;
    m_search_pending = false;
  }
  {
    std::lock_guard<std::mutex> lock(m_out_mutex);
    std::queue<SearchResult> empty;
    std::swap(m_out, empty);
  }
}

bool RecursiveSearchWorker::match_name(const std::string& name) {
  // Caller holds no locks; m_options/m_query are stable during a run.
  const int mode = m_options.mode;
  const bool cs = m_options.case_sensitive;

  switch (mode) {
    case 1: // Glob
      return fnmatch(m_query.c_str(), name.c_str(), cs ? 0 : FNM_CASEFOLD) == 0;
    case 2: // Regex
      if (!m_regex_ok) return false;
      try {
        return std::regex_search(name, m_regex);
      } catch (const std::regex_error&) {
        return false;
      }
    default: // Plain (and content-mode name fallback)
      return !m_query.empty() && plain_contains(name, m_query, cs);
  }
}

bool RecursiveSearchWorker::match_content(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return false;
  if (static_cast<uint64_t>(st.st_size) > kContentMaxBytes) return false;

  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;

  char buf[kContentMaxBytes];
  size_t n = std::fread(buf, 1, sizeof(buf), f);
  std::fclose(f);

  size_t probe = std::min(n, kBinaryProbeBytes);
  if (!looks_textual(buf, probe)) return false;

  std::string content(buf, n);
  const int mode = m_options.mode;
  const bool cs = m_options.case_sensitive;

  if (mode == 2) { // Regex over content
    if (!m_regex_ok) return false;
    try {
      return std::regex_search(content, m_regex);
    } catch (const std::regex_error&) {
      return false;
    }
  }
  return !m_query.empty() && plain_contains(content, m_query, cs);
}

void RecursiveSearchWorker::thread_main() {
  while (m_running.load()) {
    {
      std::unique_lock<std::mutex> lock(m_ctrl_mutex);
      m_cv.wait(lock, [this] {
        return m_search_pending || !m_running.load();
      });
      if (!m_running.load()) return;
      if (!m_search_pending) continue;
      m_search_pending = false;
    }

    walk_directory(m_root_dir, "", 0);

    // Signal that search is complete
    {
      std::lock_guard<std::mutex> lock(m_ctrl_mutex);
      m_search_pending = false;
    }
  }
}

void RecursiveSearchWorker::walk_directory(const std::string& dir,
                                            const std::string& rel,
                                            int depth) {
  // Check cancel
  {
    std::lock_guard<std::mutex> lock(m_ctrl_mutex);
    if (m_cancel_requested) return;
  }

  // Limit depth to avoid going too deep
  if (depth > 8) return;

  DIR* d = opendir(dir.c_str());
  if (!d) return;

  struct dirent* dent;
  while ((dent = readdir(d)) != nullptr) {
    // Check cancel periodically
    {
      std::lock_guard<std::mutex> lock(m_ctrl_mutex);
      if (m_cancel_requested) { closedir(d); return; }
    }

    std::string name = dent->d_name;
    if (name == "." || name == "..") continue;

    std::string full = dir + "/" + name;
    std::string relative = rel.empty() ? name : rel + "/" + name;

    bool is_dir = (dent->d_type == DT_DIR);

    if (is_dir && is_skip_dir(name)) continue;

    bool matched = match_name(name);
    if (!matched && m_options.mode == 3 && !is_dir)
      matched = match_content(full); // Content mode

    if (matched) {
      bool keep = true;
      if (m_options.predicate) {
        uint64_t size = 0;
        int64_t mtime = 0;
        struct stat st{};
        if (::stat(full.c_str(), &st) == 0) {
          size = static_cast<uint64_t>(st.st_size);
          mtime = static_cast<int64_t>(st.st_mtime);
        }
        keep = m_options.predicate(full, name, is_dir, size, mtime);
      }
      if (keep) {
        SearchResult r;
        r.path = full;
        r.relative_path = relative;
        r.is_dir = is_dir;
        {
          std::lock_guard<std::mutex> lock(m_out_mutex);
          m_out.push(std::move(r));
        }
      }
    }

    // Recurse into subdirectories
    if (is_dir) {
      walk_directory(full, relative, depth + 1);
    }
  }
  closedir(d);
}

RecursiveSearchWorker& recursive_search_worker() {
  static RecursiveSearchWorker instance;
  return instance;
}

} // namespace eh::file_browser
