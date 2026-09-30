#include "app/file_browser/features/recursive_search_worker/recursive_search_worker.hpp"
#include "app/file_browser/features/query_match/query_match.hpp"
#include "app/file_browser/features/tags/tags.hpp"
#include "tools/hgrep/grep.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <fnmatch.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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
    m_seen.clear();
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
    m_hgrep_cancel.store(false);
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
  m_hgrep_cancel.store(true);
  pid_t pid = m_grep_pid.load();
  if (pid > 0) ::kill(pid, SIGKILL);
  {
    std::lock_guard<std::mutex> lock(m_out_mutex);
    std::queue<SearchResult> empty;
    std::swap(m_out, empty);
    m_seen.clear();
  }
}

bool RecursiveSearchWorker::match_name(const std::string& name) {  // Caller holds no locks; m_options/m_query are stable during a run.
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

bool RecursiveSearchWorker::match_name_or_tags(const std::string& full_path,
                                               const std::string& name) {
  if (match_name(name)) return true;
  // Tags second: xattr read only happens when the name missed.
  std::string csv = read_xdg_tags(full_path);
  if (csv.empty() || m_query.empty()) return false;
  const int mode = m_options.mode;
  const bool cs = m_options.case_sensitive;
  for (auto& tag : split_tags(csv)) {
    switch (mode) {
      case 1: // Glob
        if (fnmatch(m_query.c_str(), tag.c_str(), cs ? 0 : FNM_CASEFOLD) == 0)
          return true;
        break;
      case 2: // Regex
        if (m_regex_ok) {
          try {
            if (std::regex_search(tag, m_regex)) return true;
          } catch (const std::regex_error&) {
            return false;
          }
        }
        break;
      default: // Plain
        if (plain_contains(tag, m_query, cs)) return true;
        break;
    }
  }
  return false;
}

bool RecursiveSearchWorker::match_content(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
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

bool RecursiveSearchWorker::content_via_helper(const std::string& root_dir,
                                              const std::string& query,
                                              const SearchOptions& options) {
  // Opt-out: EH_SEARCH_HELPER=0 forces the in-process hgrep path.
  if (const char* e = std::getenv("EH_SEARCH_HELPER"))
    if (*e && e[0] == '0') return false;
  // horizon-grep has no -- separator; a leading dash would parse as a flag.
  if (!query.empty() && query[0] == '-') return false;

  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) return false;

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return false;
  }
  if (pid == 0) {
    // Child: stdout -> pipe, stderr -> /dev/null, then exec helper.
    ::dup2(fds[1], STDOUT_FILENO);
    int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    ::close(fds[0]);
    ::close(fds[1]);
    if (!options.case_sensitive)
      ::execlp("horizon-grep", "horizon-grep", "-l", "-i", "-u",
               "--no-ignore", "--max-size", "50", query.c_str(),
               root_dir.c_str(), (char*)nullptr);
    else
      ::execlp("horizon-grep", "horizon-grep", "-l", "-u", "--no-ignore",
               "--max-size", "50", query.c_str(), root_dir.c_str(),
               (char*)nullptr);
    _exit(127);  // execlp failed (helper missing)
  }
  ::close(fds[1]);
  m_grep_pid.store(pid);

  // Non-blocking reads so cancel() (kill + atomic) is honored promptly.
  int flags = ::fcntl(fds[0], F_GETFL, 0);
  if (flags >= 0) ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

  std::string buf;
  buf.reserve(4096);
  char tmp[4096];
  size_t streamed = 0;
  bool cancelled = false;

  auto push_path = [&](const std::string& p) {
    std::lock_guard<std::mutex> lock(m_out_mutex);
    if (!m_seen.insert(p).second) return;  // one row/file
    std::string name;
    auto slash = p.rfind('/');
    name = (slash == std::string::npos) ? p : p.substr(slash + 1);
    bool keep = true;
    if (options.predicate) {
      uint64_t size = 0;
      int64_t mtime = 0;
      struct stat st{};
      if (::stat(p.c_str(), &st) == 0) {
        size = static_cast<uint64_t>(st.st_size);
        mtime = static_cast<int64_t>(st.st_mtime);
      }
      keep = options.predicate(p, name, false, size, mtime);
    }
    if (!keep) return;
    SearchResult r;
    r.path = p;
    std::string prefix = root_dir;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';
    r.relative_path = (p.rfind(prefix, 0) == 0) ? p.substr(prefix.size())
                                               : name;
    r.is_dir = false;
    r.tags_csv = read_xdg_tags(p);
    m_out.push(std::move(r));
    ++streamed;
  };

  for (;;) {
    if (m_hgrep_cancel.load() || !m_running.load()) {
      cancelled = true;
      break;
    }
    struct pollfd pfd{};
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 50);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;  // timeout: re-check cancel
    if (pfd.revents & (POLLIN | POLLHUP)) {
      ssize_t n = ::read(fds[0], tmp, sizeof(tmp));
      if (n > 0) {
        buf.append(tmp, static_cast<size_t>(n));
        size_t pos = 0;
        for (;;) {
          size_t nl = buf.find('\n', pos);
          if (nl == std::string::npos) break;
          std::string line = buf.substr(pos, nl - pos);
          pos = nl + 1;
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (!line.empty()) push_path(line);
        }
        buf.erase(0, pos);
      } else if (n == 0) {
        break;  // EOF
      } else if (errno != EAGAIN && errno != EINTR) {
        break;
      }
    }
    if (pfd.revents & (POLLERR | POLLNVAL)) break;
  }
  if (!buf.empty() && !cancelled) push_path(buf);

  if (cancelled) ::kill(pid, SIGKILL);
  ::close(fds[0]);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  m_grep_pid.store(-1);
  // Helper missing (exec failed) and nothing streamed: fall back in-process.
  if (!cancelled && streamed == 0 && WIFEXITED(status) &&
      WEXITSTATUS(status) == 127)
    return false;
  return true;
}

void RecursiveSearchWorker::thread_main() {
  while (m_running.load()) {
    std::string root_dir, query;
    SearchOptions options;
    {
      std::unique_lock<std::mutex> lock(m_ctrl_mutex);
      m_cv.wait(lock, [this] {
        return m_search_pending || !m_running.load();
      });
      if (!m_running.load()) return;
      if (!m_search_pending) continue;
      m_search_pending = false;
      root_dir = m_root_dir;
      query = m_query;
      options = m_options;
    }

    // Content mode: fast name+tag walk first (immediate rows), then file
    // contents. Contents prefer the out-of-process horizon-grep helper so
    // the multi-thread grep pool and file buffers live outside horizon-files
    // RSS; falls back to in-process hgrep when the helper is unavailable.
    // Both feed the same queue; paths dedupe so a file matching twice
    // still shows once.
    if (options.mode == 3 && !query.empty()) {
      walk_directory(root_dir, "", 0, true);
      if (content_via_helper(root_dir, query, options)) {
        // Helper ran (or was cancelled) — nothing more to do.
      } else {
      hgrep::GrepOptions gopt;
      gopt.pattern = query;
      gopt.case_insensitive = !options.case_sensitive;
      // File-manager parity with the legacy walker: search hidden files
      // and ignore no ignore-files (only .git internals stay pruned).
      // No depth cap and a 50 MB ceiling instead of 512 KB — strictly
      // more results than the old 8-deep walk.
      gopt.hidden = true;
      gopt.no_ignore = true;
      auto predicate = options.predicate; // stable copy for callbacks
      hgrep::GrepStats stats;
      hgrep::hgrep_search(
          root_dir, gopt,
          [&](hgrep::GrepMatch m) {
            std::lock_guard<std::mutex> lock(m_out_mutex);
            if (!m_seen.insert(m.path).second) return true; // one row/file
            std::string name;
            auto slash = m.path.rfind('/');
            name = (slash == std::string::npos) ? m.path
                                                : m.path.substr(slash + 1);
            bool keep = true;
            if (predicate) {
              uint64_t size = 0;
              int64_t mtime = 0;
              struct stat st{};
              if (::stat(m.path.c_str(), &st) == 0) {
                size = static_cast<uint64_t>(st.st_size);
                mtime = static_cast<int64_t>(st.st_mtime);
              }
              keep = predicate(m.path, name, false, size, mtime);
            }
            if (keep) {
              SearchResult r;
              r.path = m.path;
              std::string prefix = root_dir;
              if (!prefix.empty() && prefix.back() != '/') prefix += '/';
              r.relative_path =
                  (m.path.rfind(prefix, 0) == 0)
                      ? m.path.substr(prefix.size())
                      : name;
              r.is_dir = false;
              r.tags_csv = read_xdg_tags(m.path);
              m_out.push(std::move(r));
            }
            return true;
          },
          m_hgrep_cancel, stats);
      }  // end in-process fallback
    } else {
      walk_directory(m_root_dir, "", 0);
    }

    // Signal that search is complete
    {
      std::lock_guard<std::mutex> lock(m_ctrl_mutex);
      m_search_pending = false;
    }
  }
}

void RecursiveSearchWorker::walk_directory(const std::string& dir,
                                            const std::string& rel,
                                            int depth, bool names_only) {
  // Check cancel
  {
    std::lock_guard<std::mutex> lock(m_ctrl_mutex);
    if (m_cancel_requested) return;
  }

  // Limit depth to avoid going too deep (name-only passes walk fully:
  // readdir is cheap, and hgrep already covers deep content).
  if (depth > 8 && !names_only) return;

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

    bool matched = match_name_or_tags(full, name);
    if (!matched && !names_only && m_options.mode == 3 && !is_dir)
      matched = match_content(full); // Content mode (legacy single-file path)

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
        if (!is_dir) r.tags_csv = read_xdg_tags(full);
        {
          std::lock_guard<std::mutex> lock(m_out_mutex);
          m_out.push(std::move(r));
        }
      }
    }

    // Recurse into subdirectories
    if (is_dir) {
      walk_directory(full, relative, depth + 1, names_only);
    }
  }
  closedir(d);
}

RecursiveSearchWorker& recursive_search_worker() {
  static RecursiveSearchWorker instance;
  return instance;
}

} // namespace eh::file_browser
