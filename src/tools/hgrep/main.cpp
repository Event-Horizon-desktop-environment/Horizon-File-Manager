// main.cpp — horizon-grep CLI: the same core the search engine will use.

#include "grep.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [options] PATTERN [PATH]\n"
               "  -i               case-insensitive\n"
               "  -l               files with matches only\n"
               "  -c               match counts per file\n"
               "  -u               search hidden files/dirs (except .git)\n"
               "  --no-ignore      ignore .gitignore/.ignore rules\n"
               "  -L               follow symlinked dirs\n"
               "  -j N             worker threads (default: all cores)\n"
               "  --max-size MB    skip files larger than MB (default 50)\n"
               "  --stats          print files/bytes/matches to stderr\n",
               argv0);
}

} // namespace

int main(int argc, char** argv) {
  hgrep::GrepOptions opt;
  const char* pattern = nullptr;
  const char* path = ".";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-i") opt.case_insensitive = true;
    else if (a == "-l") opt.files_only = true;
    else if (a == "-c") opt.count_only = true;
    else if (a == "-u") opt.hidden = true;
    else if (a == "--no-ignore") opt.no_ignore = true;
    else if (a == "-L") opt.follow_symlink_dirs = true;
    else if (a == "-j" && i + 1 < argc) opt.threads = std::atoi(argv[++i]);
    else if (a == "--max-size" && i + 1 < argc)
      opt.max_filesize = static_cast<uint64_t>(std::atoll(argv[++i])) *
                         1024u * 1024u;
    else if (a == "--stats") {
      // handled below via env-free flag scan
    } else if (a == "-h" || a == "--help") {
      usage(argv[0]);
      return 0;
    } else if (!pattern) {
      pattern = argv[i];
    } else {
      path = argv[i];
    }
  }
  bool show_stats = false;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--stats") show_stats = true;
  if (!pattern) {
    usage(argv[0]);
    return 2;
  }
  opt.pattern = pattern;
  if (opt.files_only && opt.count_only) {
    std::fprintf(stderr, "horizon-grep: -l and -c are exclusive\n");
    return 2;
  }
  std::atomic<bool> cancel{false};
  hgrep::GrepStats stats;
  // Single shared stdout lock lives inside the core (emit_mtx); print
  // directly from the callback.
  bool ok = hgrep::hgrep_search(
      path, opt,
      [&](hgrep::GrepMatch m) {
        if (opt.count_only)
          std::printf("%s:%llu\n", m.path.c_str(),
                      (unsigned long long)m.line_no);
        else if (opt.files_only)
          std::printf("%s\n", m.path.c_str());
        else if (m.line_no == 0 && m.line.empty())
          std::printf("%s: binary file matches\n", m.path.c_str());
        else
          std::printf("%s:%llu:%s\n", m.path.c_str(),
                      (unsigned long long)m.line_no, m.line.c_str());
        return true;
      },
      cancel, stats);
  if (show_stats)
    std::fprintf(stderr,
                 "files=%llu skipped=%llu bytes=%llu matches=%llu threads=%d\n",
                 (unsigned long long)stats.files_searched,
                 (unsigned long long)stats.files_skipped,
                 (unsigned long long)stats.bytes_searched,
                 (unsigned long long)stats.matches,
                 opt.threads > 0 ? opt.threads
                                 : static_cast<int>(
                                       std::thread::hardware_concurrency()));
  return ok ? 0 : 1;
}
